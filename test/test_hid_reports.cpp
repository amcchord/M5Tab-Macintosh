#include "hid_descriptor.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    // Report 1 has unrelated buttons. Report 2 owns movement and different buttons.
    const uint8_t descriptor[] = {
        0x85,1, 0x05,9, 0x09,1, 0x15,0, 0x25,1, 0x75,1, 0x95,8, 0x81,2,
        0x85,2, 0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f,
        0x75,8, 0x95,2, 0x81,6,
        0x05,9, 0x09,1, 0x15,0, 0x25,1, 0x75,1, 0x95,8, 0x81,2,
        0x85,3, 0x05,1, 0x09,0x38, 0x15,0x81, 0x25,0x7f, 0x75,8, 0x95,1, 0x81,6
    };
    HidMouseLayout layout;
    assert(HidParseMouseDescriptor(descriptor,sizeof(descriptor),&layout));
    assert(layout.report_id==2 && layout.report_bytes==4 && !layout.has_wheel);
    assert(layout.button_offset_bits==16);
    uint8_t buttons=99; int16_t x=99,y=99,w=99;
    const uint8_t valid[]={2,5,0xfe,3};
    assert(HidDecodeMouseReport(valid,sizeof(valid),&layout,&buttons,&x,&y,&w));
    assert(buttons==3 && x==5 && y==-2 && w==0);
    buttons=x=y=w=99;
    assert(!HidDecodeMouseReport(valid,2,&layout,&buttons,&x,&y,&w));
    assert(buttons==99 && x==99 && y==99 && w==99);
    const uint8_t unrelated[]={3,20,10,0};
    assert(!HidDecodeMouseReport(unrelated,sizeof(unrelated),&layout,&buttons,&x,&y,&w));
    assert(buttons==99 && x==99 && y==99 && w==99);
    // X in one report and Y in another is not a complete mouse layout.
    const uint8_t split[]={0x05,1,0x75,8,0x95,1,0x85,1,0x09,0x30,0x81,6,
                          0x85,2,0x09,0x31,0x81,6};
    assert(!HidParseMouseDescriptor(split,sizeof(split),&layout) && !layout.valid);
    // A normal boot mouse still works with signed axes and no Report ID.
    const uint8_t boot[]={0x05,9,0x09,1,0x75,1,0x95,8,0x81,2,
                         0x05,1,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6};
    assert(HidParseMouseDescriptor(boot,sizeof(boot),&layout));
    assert(layout.report_id==0 && layout.report_bytes==3);
    const uint8_t report[]={1,0xff,4};
    assert(HidDecodeMouseReport(report,sizeof(report),&layout,&buttons,&x,&y,&w));
    assert(buttons==1 && x==-1 && y==4);
    puts("PASS: independent HID report streams, truncated packets and signed boot mouse movement");
}
