#!/usr/bin/env python3
"""Measure identical, verified Finder selection drags on a disposable 640x360 guest disk."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import time

from mac_control import ControlError, MacControl
from speedometer_benchmark import _pixel, _png_rgb_rows, capture


# SimAnt Finder folder, icon view, upper-left window. Keep the trace in empty space.
WIDTH, HEIGHT = 640, 360
CONTENTS = (25, 70, 395, 240)
TITLE = (35, 28, 396, 43)
STRIP = (0, 337, 310, 360)
START = (92, 122)
PARK = (608, 215)
END = (165, 143)
PERIOD = 0.25
# Both horizontal edges stay in the same 40-row tile band (120..159).
# This creates a real gap for the sparse-row renderer in interior tile columns.
PATH = tuple((END[0] + 7 * min(i % 32, 31 - i % 32),
              END[1] + min(i % 32, 31 - i % 32)) for i in range(96))


def region(rows, bounds):
    x0, y0, x1, y1 = bounds
    stride = WIDTH * 3 + 1
    return b''.join(rows[y*stride+1+x0*3:y*stride+1+x1*3] for y in range(y0, y1))


def read_scene(path):
    w, h, rows = _png_rgb_rows(path)
    if (w, h) != (WIDTH, HEIGHT):
        raise ValueError('The Finder trace requires a 640 x 360 guest screenshot.')
    return rows


def scene_signature(rows):
    return hashlib.sha256(b''.join(region(rows, box) for box in (CONTENTS, TITLE, STRIP))).hexdigest()


def grip_matches(a, ax, b, bx):
    # The right-hand resize tab is at x=0 when collapsed and x=291 when expanded.
    lhs = region(a, (ax, 338, ax+13, 355))
    rhs = region(b, (bx, 338, bx+13, 355))
    return sum(x != y for x, y in zip(lhs, rhs)) <= 6


def point_changed(before, held, x, y):
    return _pixel(before, WIDTH, x, y) != _pixel(held, WIDTH, x, y)


def verify_outline(before, held):
    x0, y0 = START
    x1, y1 = END
    # Allow endpoint convention and the alternating phase of Finder's dashed outline.
    # Require all four sides and a clear interior, independently of the cursor.
    for x in (x0, x1):
        coords = range(y0+4, y1-4)
        if max(sum(point_changed(before, held, x+d, y) for y in coords)
               for d in range(-2, 3)) < len(coords)*0.3:
            raise ValueError('Finder did not draw the expected vertical selection edge.')
    for y in (y0, y1):
        coords = range(x0+5, x1-5)
        if max(sum(point_changed(before, held, x, y+d) for x in coords)
               for d in range(-2, 3)) < len(coords)*0.3:
            raise ValueError('Finder did not draw the expected horizontal selection edge.')
    if region(before, (x0+5,y0+5,x1-5,y1-5)) != region(held, (x0+5,y0+5,x1-5,y1-5)):
        raise ValueError('The Finder selection interior changed.')


class Counters:
    def __init__(self):
        self.frames = None
        self.records = []

    def accept(self, data, now):
        header = re.search(rb'frames=(\d+) \(full=(\d+) partial=(\d+) skip=(\d+)\)', data)
        if header:
            self.frames = (now, dict(zip(('frames','full','partial','skip'), map(int, header.groups()))))
        averages = re.search(rb'detect=(\d+)us render=(\d+)us panel_bytes=(\d+)', data)
        if averages:
            record = dict(zip(('detect_us','render_us','panel_bytes'), map(int, averages.groups())))
            if self.frames and now - self.frames[0] < 2:
                record.update(self.frames[1])
            self.records.append(dict(at=now, **record))
            self.frames = None


class TraceControl(MacControl):
    def __init__(self, port):
        super().__init__(port, reset_on_failure=False)
        self.counters = Counters()

    def _protocol_fragments(self, raw):
        self.counters.accept(raw, time.monotonic())
        return super()._protocol_fragments(raw)

    def listen(self, deadline):
        for _ in self._protocol_lines(deadline):
            pass

    def next_boundary(self):
        start = len(self.counters.records)
        deadline = time.monotonic() + 42
        while time.monotonic() < deadline:
            self.listen(min(deadline, time.monotonic() + .05))
            if len(self.counters.records) > start:
                return self.counters.records[-1]
        raise ValueError('No display counter boundary in 42 s. Check that VIDEO PERF is enabled.')


def measure_interval(control, start):
    records = control.counters.records
    before = len(records)
    issue_times = []
    for i, point in enumerate(PATH):
        due = start['at'] + 0.5 + i*PERIOD
        control.listen(due)
        # Never catch up with a burst of moves or accept a partial interval.
        if len(records) != before or time.monotonic() - due > PERIOD/2:
            raise ValueError('USB missed the movement deadline. Repeat this run.')
        issue_times.append(time.monotonic())
        control.move(*point)
    if len(records) != before:
        raise ValueError('Display counters rolled over during the movement sequence.')
    end = control.next_boundary()
    span = end['at'] - start['at']
    if not 28.5 < span < 31.5:
        raise ValueError('A display counter interval was lost or delayed.')
    return end, dict(end, seconds=round(span, 3), moves=len(PATH),
                     max_period_error_s=round(max(abs(b-a-PERIOD)
                                                 for a,b in zip(issue_times, issue_times[1:])), 4))


def valid_sample(sample):
    return (sample.get('full') == 0 and sample['frames'] == sample['partial']+sample['skip']
            and sample['partial'] >= len(PATH) - 12)


def compare(baseline, candidate):
    for key in ('scene_sha256', 'trace_sha256', 'disk'):
        if baseline[key] != candidate[key]:
            raise ValueError(f'Cannot compare different {key} values.')
    a, b = baseline['samples'], candidate['samples']
    if not a or not b or not all(valid_sample(s) for s in a+b):
        raise ValueError('Incomplete or invalid samples.')
    for metric in ('frames', 'partial'):
        med_a = statistics.median(s[metric] for s in a)
        med_b = statistics.median(s[metric] for s in b)
        if abs(med_a-med_b) > max(4, .06*med_a):
            raise ValueError(f'Guest workload differs ({metric} medians {med_a} and {med_b}).')
    return {metric: [statistics.median(s[metric] for s in samples) for samples in (a,b)]
            for metric in ('partial','frames','detect_us','render_us','panel_bytes')}


def run(args):
    reference = read_scene(args.reference)
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    with (out/'moves.txt').open('w') as trace:
        for x, y in (START, END, *PATH):
            trace.write(f'{x} {y}\n')

    def shot(name):
        return read_scene(capture(control, out/(name+'.png'), color=True))

    with TraceControl(args.port) as control:
        control.connect(timeout=15)
        if control.boot_get('disk') != args.disk or control.boot_status()['phase'] != 'emulator':
            raise ValueError('Start the specified disposable Finder disk before running.')
        control.move(*PARK)
        time.sleep(.35)
        initial = shot('initial')
        if region(initial, CONTENTS) != region(reference, CONTENTS):
            raise ValueError('Finder window contents differ from the supplied reference.')
        if region(initial, STRIP) != region(reference, STRIP):
            if grip_matches(initial, 0, reference, 291):
                control.click(8, 347)
            elif grip_matches(initial, 291, reference, 0):
                control.click(299, 347)
            else:
                raise ValueError('The Mac OS Control Strip differs from the reference.')
            time.sleep(.7)
        control.click(*START)
        time.sleep(.55)
        control.move(*PARK)
        time.sleep(.35)
        focused = shot('focused')
        if scene_signature(focused) != scene_signature(reference):
            raise ValueError('Finder focus or Control Strip still differs from the reference.')
        samples = []
        rejected = []
        try:
            control.move(*START)
            time.sleep(.3)
            control.mouse_button(0, True)
            time.sleep(.3)
            control.move(*END)
            time.sleep(.4)
            verify_outline(focused, shot('held-before'))
            boundary = control.next_boundary()  # discard startup, focusing and screenshot traffic
            for _ in range(args.intervals + 4):
                boundary, sample = measure_interval(control, boundary)
                (samples if valid_sample(sample) else rejected).append(sample)
                print(json.dumps(sample), flush=True)
                if len(samples) >= args.intervals:
                    break
            verify_outline(focused, shot('held-after'))
        finally:
            control.mouse_button(0, False)
            control.move(*PARK)
        time.sleep(.4)
        if scene_signature(shot('released')) != scene_signature(reference):
            raise ValueError('Finder did not return to the starting scene.')
        panel = control.panel_verify()
        if not panel['checked'] or panel['mismatched']:
            raise ValueError(f'Physical panel readback failed: {panel}')
        report = dict(disk=args.disk, scene_sha256=scene_signature(focused),
                      trace_sha256=hashlib.sha256(repr((START, PATH, PERIOD)).encode()).hexdigest(),
                      session=control.session_id, samples=samples, rejected=rejected, panel=panel)
        (out/'trace.json').write_text(json.dumps(report, indent=2)+'\n')
        if len(samples) < args.intervals:
            raise ValueError('Too few intervals with complete frame counters and the required partial redraws.')
        if args.compare:
            summary = compare(json.loads(args.compare.read_text()), report)
            print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--disk', required=True, help='exact disposable startup disk path on the card')
    parser.add_argument('--reference', type=Path, required=True, help='MacControl color screenshot of the focused Finder scene')
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory')
    parser.add_argument('--intervals', type=int, default=3, choices=range(1, 13))
    parser.add_argument('--compare', type=Path, help='baseline trace.json')
    args = parser.parse_args()
    try:
        run(args)
    except (ControlError, OSError, ValueError) as exc:
        parser.exit(1, f'Trace stopped: {exc}\n')


if __name__ == '__main__':
    main()
