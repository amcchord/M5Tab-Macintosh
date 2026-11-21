from pathlib import Path
import sys
from unittest import TestCase, mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import finder_display_benchmark as bench


class FinderTraceTests(TestCase):
    def test_four_edges_and_clear_interior(self):
        size = (bench.WIDTH * 3 + 1) * bench.HEIGHT
        before, held = bytes(size), bytearray(size)
        def draw(x, y):
            offset = y * (bench.WIDTH * 3 + 1) + 1 + 3 * x
            held[offset:offset+3] = b'\xff\xff\xff'
        left, top = bench.START
        right, bottom = bench.END
        for x in range(left, right + 1, 2):
            draw(x, top)
            draw(x, bottom)
        with self.assertRaises(ValueError):
            bench.verify_outline(before, held)
        for y in range(top, bottom + 1, 2):
            draw(left, y)
            draw(right, y)
        bench.verify_outline(before, held)
        draw(left+6, top+6)
        with self.assertRaises(ValueError):
            bench.verify_outline(before, held)

    def test_fragmented_counters_never_reuse_an_old_header(self):
        counters = bench.Counters()
        counters.accept(b'noise[VIDEO PERF] frames=660 (full=0 partial=110 skip=550)', 1.)
        counters.accept(b'other detect=62us render=180us panel_bytes=1234', 1.01)
        self.assertTrue(bench.valid_sample(counters.records[-1]))
        counters.accept(b'other detect=69us render=160us panel_bytes=1000', 31.)
        self.assertFalse(bench.valid_sample(counters.records[-1]))
        counters.accept(b'noise[VIDEO PERF] frames=660 (full=1 partial=110 skip=549)', 61.)
        counters.accept(b'other detect=69us render=160us panel_bytes=1000', 61.1)
        self.assertFalse(bench.valid_sample(counters.records[-1]))

    def test_fixed_moves_and_lost_deadline(self):
        class Device:
            def __init__(self, latency):
                self.latency, self.now = latency, 0.
                self.counters, self.points = bench.Counters(), []
            def listen(self, deadline):
                self.now = max(self.now, deadline)
            def move(self, x, y):
                self.points.append((x,y))
                self.now += self.latency
            def next_boundary(self):
                return dict(at=30.)
        model = Device(.03)
        with mock.patch.object(bench.time, 'monotonic', side_effect=lambda: model.now):
            _, sample = bench.measure_interval(model, dict(at=0.))
            self.assertEqual(tuple(model.points), bench.PATH)
            self.assertEqual(sample['moves'], 96)
            self.assertEqual(sample['max_period_error_s'], 0.)
            model = Device(.5)
            with self.assertRaisesRegex(ValueError, 'deadline'):
                bench.measure_interval(model, dict(at=0.))
            self.assertEqual(len(model.points), 1)

    def test_comparison_requires_matching_scene_and_frame_counts(self):
        sample = dict(frames=660, partial=110, skip=550, full=0, detect_us=60, render_us=150, panel_bytes=20000)
        baseline = dict(disk='/scratch.dsk', trace_sha256='trace', scene_sha256='scene', samples=[sample])
        self.assertEqual(bench.compare(baseline, baseline)['panel_bytes'], [20000,20000])
        candidate = dict(baseline, samples=[dict(sample, partial=170, skip=490)])
        with self.assertRaisesRegex(ValueError,'workload'):
            bench.compare(baseline, candidate)
        with self.assertRaisesRegex(ValueError,'scene'):
            bench.compare(baseline, dict(baseline, scene_sha256='other'))
