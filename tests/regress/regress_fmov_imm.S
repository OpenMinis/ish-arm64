#ifdef __APPLE__
#define NAME(x) _##x
#else
#define NAME(x) x
#endif
// The instructions under test live here rather than in inline asm: with inline
// asm the compiler is free to insert code between the fmov and the store, and
// may even constant-fold the immediate itself, which would test nothing.
.text
.global NAME(fmov_imm_d_table)
.align 4
NAME(fmov_imm_d_table):
  .inst 0x1e601000   // fmov d0, #imm8=0x00
  str d0, [x0, #0]
  .inst 0x1e603000   // fmov d0, #imm8=0x01
  str d0, [x0, #8]
  .inst 0x1e605000   // fmov d0, #imm8=0x02
  str d0, [x0, #16]
  .inst 0x1e607000   // fmov d0, #imm8=0x03
  str d0, [x0, #24]
  .inst 0x1e609000   // fmov d0, #imm8=0x04
  str d0, [x0, #32]
  .inst 0x1e60b000   // fmov d0, #imm8=0x05
  str d0, [x0, #40]
  .inst 0x1e60d000   // fmov d0, #imm8=0x06
  str d0, [x0, #48]
  .inst 0x1e60f000   // fmov d0, #imm8=0x07
  str d0, [x0, #56]
  .inst 0x1e611000   // fmov d0, #imm8=0x08
  str d0, [x0, #64]
  .inst 0x1e613000   // fmov d0, #imm8=0x09
  str d0, [x0, #72]
  .inst 0x1e615000   // fmov d0, #imm8=0x0a
  str d0, [x0, #80]
  .inst 0x1e617000   // fmov d0, #imm8=0x0b
  str d0, [x0, #88]
  .inst 0x1e619000   // fmov d0, #imm8=0x0c
  str d0, [x0, #96]
  .inst 0x1e61b000   // fmov d0, #imm8=0x0d
  str d0, [x0, #104]
  .inst 0x1e61d000   // fmov d0, #imm8=0x0e
  str d0, [x0, #112]
  .inst 0x1e61f000   // fmov d0, #imm8=0x0f
  str d0, [x0, #120]
  .inst 0x1e621000   // fmov d0, #imm8=0x10
  str d0, [x0, #128]
  .inst 0x1e623000   // fmov d0, #imm8=0x11
  str d0, [x0, #136]
  .inst 0x1e625000   // fmov d0, #imm8=0x12
  str d0, [x0, #144]
  .inst 0x1e627000   // fmov d0, #imm8=0x13
  str d0, [x0, #152]
  .inst 0x1e629000   // fmov d0, #imm8=0x14
  str d0, [x0, #160]
  .inst 0x1e62b000   // fmov d0, #imm8=0x15
  str d0, [x0, #168]
  .inst 0x1e62d000   // fmov d0, #imm8=0x16
  str d0, [x0, #176]
  .inst 0x1e62f000   // fmov d0, #imm8=0x17
  str d0, [x0, #184]
  .inst 0x1e631000   // fmov d0, #imm8=0x18
  str d0, [x0, #192]
  .inst 0x1e633000   // fmov d0, #imm8=0x19
  str d0, [x0, #200]
  .inst 0x1e635000   // fmov d0, #imm8=0x1a
  str d0, [x0, #208]
  .inst 0x1e637000   // fmov d0, #imm8=0x1b
  str d0, [x0, #216]
  .inst 0x1e639000   // fmov d0, #imm8=0x1c
  str d0, [x0, #224]
  .inst 0x1e63b000   // fmov d0, #imm8=0x1d
  str d0, [x0, #232]
  .inst 0x1e63d000   // fmov d0, #imm8=0x1e
  str d0, [x0, #240]
  .inst 0x1e63f000   // fmov d0, #imm8=0x1f
  str d0, [x0, #248]
  .inst 0x1e641000   // fmov d0, #imm8=0x20
  str d0, [x0, #256]
  .inst 0x1e643000   // fmov d0, #imm8=0x21
  str d0, [x0, #264]
  .inst 0x1e645000   // fmov d0, #imm8=0x22
  str d0, [x0, #272]
  .inst 0x1e647000   // fmov d0, #imm8=0x23
  str d0, [x0, #280]
  .inst 0x1e649000   // fmov d0, #imm8=0x24
  str d0, [x0, #288]
  .inst 0x1e64b000   // fmov d0, #imm8=0x25
  str d0, [x0, #296]
  .inst 0x1e64d000   // fmov d0, #imm8=0x26
  str d0, [x0, #304]
  .inst 0x1e64f000   // fmov d0, #imm8=0x27
  str d0, [x0, #312]
  .inst 0x1e651000   // fmov d0, #imm8=0x28
  str d0, [x0, #320]
  .inst 0x1e653000   // fmov d0, #imm8=0x29
  str d0, [x0, #328]
  .inst 0x1e655000   // fmov d0, #imm8=0x2a
  str d0, [x0, #336]
  .inst 0x1e657000   // fmov d0, #imm8=0x2b
  str d0, [x0, #344]
  .inst 0x1e659000   // fmov d0, #imm8=0x2c
  str d0, [x0, #352]
  .inst 0x1e65b000   // fmov d0, #imm8=0x2d
  str d0, [x0, #360]
  .inst 0x1e65d000   // fmov d0, #imm8=0x2e
  str d0, [x0, #368]
  .inst 0x1e65f000   // fmov d0, #imm8=0x2f
  str d0, [x0, #376]
  .inst 0x1e661000   // fmov d0, #imm8=0x30
  str d0, [x0, #384]
  .inst 0x1e663000   // fmov d0, #imm8=0x31
  str d0, [x0, #392]
  .inst 0x1e665000   // fmov d0, #imm8=0x32
  str d0, [x0, #400]
  .inst 0x1e667000   // fmov d0, #imm8=0x33
  str d0, [x0, #408]
  .inst 0x1e669000   // fmov d0, #imm8=0x34
  str d0, [x0, #416]
  .inst 0x1e66b000   // fmov d0, #imm8=0x35
  str d0, [x0, #424]
  .inst 0x1e66d000   // fmov d0, #imm8=0x36
  str d0, [x0, #432]
  .inst 0x1e66f000   // fmov d0, #imm8=0x37
  str d0, [x0, #440]
  .inst 0x1e671000   // fmov d0, #imm8=0x38
  str d0, [x0, #448]
  .inst 0x1e673000   // fmov d0, #imm8=0x39
  str d0, [x0, #456]
  .inst 0x1e675000   // fmov d0, #imm8=0x3a
  str d0, [x0, #464]
  .inst 0x1e677000   // fmov d0, #imm8=0x3b
  str d0, [x0, #472]
  .inst 0x1e679000   // fmov d0, #imm8=0x3c
  str d0, [x0, #480]
  .inst 0x1e67b000   // fmov d0, #imm8=0x3d
  str d0, [x0, #488]
  .inst 0x1e67d000   // fmov d0, #imm8=0x3e
  str d0, [x0, #496]
  .inst 0x1e67f000   // fmov d0, #imm8=0x3f
  str d0, [x0, #504]
  .inst 0x1e681000   // fmov d0, #imm8=0x40
  str d0, [x0, #512]
  .inst 0x1e683000   // fmov d0, #imm8=0x41
  str d0, [x0, #520]
  .inst 0x1e685000   // fmov d0, #imm8=0x42
  str d0, [x0, #528]
  .inst 0x1e687000   // fmov d0, #imm8=0x43
  str d0, [x0, #536]
  .inst 0x1e689000   // fmov d0, #imm8=0x44
  str d0, [x0, #544]
  .inst 0x1e68b000   // fmov d0, #imm8=0x45
  str d0, [x0, #552]
  .inst 0x1e68d000   // fmov d0, #imm8=0x46
  str d0, [x0, #560]
  .inst 0x1e68f000   // fmov d0, #imm8=0x47
  str d0, [x0, #568]
  .inst 0x1e691000   // fmov d0, #imm8=0x48
  str d0, [x0, #576]
  .inst 0x1e693000   // fmov d0, #imm8=0x49
  str d0, [x0, #584]
  .inst 0x1e695000   // fmov d0, #imm8=0x4a
  str d0, [x0, #592]
  .inst 0x1e697000   // fmov d0, #imm8=0x4b
  str d0, [x0, #600]
  .inst 0x1e699000   // fmov d0, #imm8=0x4c
  str d0, [x0, #608]
  .inst 0x1e69b000   // fmov d0, #imm8=0x4d
  str d0, [x0, #616]
  .inst 0x1e69d000   // fmov d0, #imm8=0x4e
  str d0, [x0, #624]
  .inst 0x1e69f000   // fmov d0, #imm8=0x4f
  str d0, [x0, #632]
  .inst 0x1e6a1000   // fmov d0, #imm8=0x50
  str d0, [x0, #640]
  .inst 0x1e6a3000   // fmov d0, #imm8=0x51
  str d0, [x0, #648]
  .inst 0x1e6a5000   // fmov d0, #imm8=0x52
  str d0, [x0, #656]
  .inst 0x1e6a7000   // fmov d0, #imm8=0x53
  str d0, [x0, #664]
  .inst 0x1e6a9000   // fmov d0, #imm8=0x54
  str d0, [x0, #672]
  .inst 0x1e6ab000   // fmov d0, #imm8=0x55
  str d0, [x0, #680]
  .inst 0x1e6ad000   // fmov d0, #imm8=0x56
  str d0, [x0, #688]
  .inst 0x1e6af000   // fmov d0, #imm8=0x57
  str d0, [x0, #696]
  .inst 0x1e6b1000   // fmov d0, #imm8=0x58
  str d0, [x0, #704]
  .inst 0x1e6b3000   // fmov d0, #imm8=0x59
  str d0, [x0, #712]
  .inst 0x1e6b5000   // fmov d0, #imm8=0x5a
  str d0, [x0, #720]
  .inst 0x1e6b7000   // fmov d0, #imm8=0x5b
  str d0, [x0, #728]
  .inst 0x1e6b9000   // fmov d0, #imm8=0x5c
  str d0, [x0, #736]
  .inst 0x1e6bb000   // fmov d0, #imm8=0x5d
  str d0, [x0, #744]
  .inst 0x1e6bd000   // fmov d0, #imm8=0x5e
  str d0, [x0, #752]
  .inst 0x1e6bf000   // fmov d0, #imm8=0x5f
  str d0, [x0, #760]
  .inst 0x1e6c1000   // fmov d0, #imm8=0x60
  str d0, [x0, #768]
  .inst 0x1e6c3000   // fmov d0, #imm8=0x61
  str d0, [x0, #776]
  .inst 0x1e6c5000   // fmov d0, #imm8=0x62
  str d0, [x0, #784]
  .inst 0x1e6c7000   // fmov d0, #imm8=0x63
  str d0, [x0, #792]
  .inst 0x1e6c9000   // fmov d0, #imm8=0x64
  str d0, [x0, #800]
  .inst 0x1e6cb000   // fmov d0, #imm8=0x65
  str d0, [x0, #808]
  .inst 0x1e6cd000   // fmov d0, #imm8=0x66
  str d0, [x0, #816]
  .inst 0x1e6cf000   // fmov d0, #imm8=0x67
  str d0, [x0, #824]
  .inst 0x1e6d1000   // fmov d0, #imm8=0x68
  str d0, [x0, #832]
  .inst 0x1e6d3000   // fmov d0, #imm8=0x69
  str d0, [x0, #840]
  .inst 0x1e6d5000   // fmov d0, #imm8=0x6a
  str d0, [x0, #848]
  .inst 0x1e6d7000   // fmov d0, #imm8=0x6b
  str d0, [x0, #856]
  .inst 0x1e6d9000   // fmov d0, #imm8=0x6c
  str d0, [x0, #864]
  .inst 0x1e6db000   // fmov d0, #imm8=0x6d
  str d0, [x0, #872]
  .inst 0x1e6dd000   // fmov d0, #imm8=0x6e
  str d0, [x0, #880]
  .inst 0x1e6df000   // fmov d0, #imm8=0x6f
  str d0, [x0, #888]
  .inst 0x1e6e1000   // fmov d0, #imm8=0x70
  str d0, [x0, #896]
  .inst 0x1e6e3000   // fmov d0, #imm8=0x71
  str d0, [x0, #904]
  .inst 0x1e6e5000   // fmov d0, #imm8=0x72
  str d0, [x0, #912]
  .inst 0x1e6e7000   // fmov d0, #imm8=0x73
  str d0, [x0, #920]
  .inst 0x1e6e9000   // fmov d0, #imm8=0x74
  str d0, [x0, #928]
  .inst 0x1e6eb000   // fmov d0, #imm8=0x75
  str d0, [x0, #936]
  .inst 0x1e6ed000   // fmov d0, #imm8=0x76
  str d0, [x0, #944]
  .inst 0x1e6ef000   // fmov d0, #imm8=0x77
  str d0, [x0, #952]
  .inst 0x1e6f1000   // fmov d0, #imm8=0x78
  str d0, [x0, #960]
  .inst 0x1e6f3000   // fmov d0, #imm8=0x79
  str d0, [x0, #968]
  .inst 0x1e6f5000   // fmov d0, #imm8=0x7a
  str d0, [x0, #976]
  .inst 0x1e6f7000   // fmov d0, #imm8=0x7b
  str d0, [x0, #984]
  .inst 0x1e6f9000   // fmov d0, #imm8=0x7c
  str d0, [x0, #992]
  .inst 0x1e6fb000   // fmov d0, #imm8=0x7d
  str d0, [x0, #1000]
  .inst 0x1e6fd000   // fmov d0, #imm8=0x7e
  str d0, [x0, #1008]
  .inst 0x1e6ff000   // fmov d0, #imm8=0x7f
  str d0, [x0, #1016]
  .inst 0x1e701000   // fmov d0, #imm8=0x80
  str d0, [x0, #1024]
  .inst 0x1e703000   // fmov d0, #imm8=0x81
  str d0, [x0, #1032]
  .inst 0x1e705000   // fmov d0, #imm8=0x82
  str d0, [x0, #1040]
  .inst 0x1e707000   // fmov d0, #imm8=0x83
  str d0, [x0, #1048]
  .inst 0x1e709000   // fmov d0, #imm8=0x84
  str d0, [x0, #1056]
  .inst 0x1e70b000   // fmov d0, #imm8=0x85
  str d0, [x0, #1064]
  .inst 0x1e70d000   // fmov d0, #imm8=0x86
  str d0, [x0, #1072]
  .inst 0x1e70f000   // fmov d0, #imm8=0x87
  str d0, [x0, #1080]
  .inst 0x1e711000   // fmov d0, #imm8=0x88
  str d0, [x0, #1088]
  .inst 0x1e713000   // fmov d0, #imm8=0x89
  str d0, [x0, #1096]
  .inst 0x1e715000   // fmov d0, #imm8=0x8a
  str d0, [x0, #1104]
  .inst 0x1e717000   // fmov d0, #imm8=0x8b
  str d0, [x0, #1112]
  .inst 0x1e719000   // fmov d0, #imm8=0x8c
  str d0, [x0, #1120]
  .inst 0x1e71b000   // fmov d0, #imm8=0x8d
  str d0, [x0, #1128]
  .inst 0x1e71d000   // fmov d0, #imm8=0x8e
  str d0, [x0, #1136]
  .inst 0x1e71f000   // fmov d0, #imm8=0x8f
  str d0, [x0, #1144]
  .inst 0x1e721000   // fmov d0, #imm8=0x90
  str d0, [x0, #1152]
  .inst 0x1e723000   // fmov d0, #imm8=0x91
  str d0, [x0, #1160]
  .inst 0x1e725000   // fmov d0, #imm8=0x92
  str d0, [x0, #1168]
  .inst 0x1e727000   // fmov d0, #imm8=0x93
  str d0, [x0, #1176]
  .inst 0x1e729000   // fmov d0, #imm8=0x94
  str d0, [x0, #1184]
  .inst 0x1e72b000   // fmov d0, #imm8=0x95
  str d0, [x0, #1192]
  .inst 0x1e72d000   // fmov d0, #imm8=0x96
  str d0, [x0, #1200]
  .inst 0x1e72f000   // fmov d0, #imm8=0x97
  str d0, [x0, #1208]
  .inst 0x1e731000   // fmov d0, #imm8=0x98
  str d0, [x0, #1216]
  .inst 0x1e733000   // fmov d0, #imm8=0x99
  str d0, [x0, #1224]
  .inst 0x1e735000   // fmov d0, #imm8=0x9a
  str d0, [x0, #1232]
  .inst 0x1e737000   // fmov d0, #imm8=0x9b
  str d0, [x0, #1240]
  .inst 0x1e739000   // fmov d0, #imm8=0x9c
  str d0, [x0, #1248]
  .inst 0x1e73b000   // fmov d0, #imm8=0x9d
  str d0, [x0, #1256]
  .inst 0x1e73d000   // fmov d0, #imm8=0x9e
  str d0, [x0, #1264]
  .inst 0x1e73f000   // fmov d0, #imm8=0x9f
  str d0, [x0, #1272]
  .inst 0x1e741000   // fmov d0, #imm8=0xa0
  str d0, [x0, #1280]
  .inst 0x1e743000   // fmov d0, #imm8=0xa1
  str d0, [x0, #1288]
  .inst 0x1e745000   // fmov d0, #imm8=0xa2
  str d0, [x0, #1296]
  .inst 0x1e747000   // fmov d0, #imm8=0xa3
  str d0, [x0, #1304]
  .inst 0x1e749000   // fmov d0, #imm8=0xa4
  str d0, [x0, #1312]
  .inst 0x1e74b000   // fmov d0, #imm8=0xa5
  str d0, [x0, #1320]
  .inst 0x1e74d000   // fmov d0, #imm8=0xa6
  str d0, [x0, #1328]
  .inst 0x1e74f000   // fmov d0, #imm8=0xa7
  str d0, [x0, #1336]
  .inst 0x1e751000   // fmov d0, #imm8=0xa8
  str d0, [x0, #1344]
  .inst 0x1e753000   // fmov d0, #imm8=0xa9
  str d0, [x0, #1352]
  .inst 0x1e755000   // fmov d0, #imm8=0xaa
  str d0, [x0, #1360]
  .inst 0x1e757000   // fmov d0, #imm8=0xab
  str d0, [x0, #1368]
  .inst 0x1e759000   // fmov d0, #imm8=0xac
  str d0, [x0, #1376]
  .inst 0x1e75b000   // fmov d0, #imm8=0xad
  str d0, [x0, #1384]
  .inst 0x1e75d000   // fmov d0, #imm8=0xae
  str d0, [x0, #1392]
  .inst 0x1e75f000   // fmov d0, #imm8=0xaf
  str d0, [x0, #1400]
  .inst 0x1e761000   // fmov d0, #imm8=0xb0
  str d0, [x0, #1408]
  .inst 0x1e763000   // fmov d0, #imm8=0xb1
  str d0, [x0, #1416]
  .inst 0x1e765000   // fmov d0, #imm8=0xb2
  str d0, [x0, #1424]
  .inst 0x1e767000   // fmov d0, #imm8=0xb3
  str d0, [x0, #1432]
  .inst 0x1e769000   // fmov d0, #imm8=0xb4
  str d0, [x0, #1440]
  .inst 0x1e76b000   // fmov d0, #imm8=0xb5
  str d0, [x0, #1448]
  .inst 0x1e76d000   // fmov d0, #imm8=0xb6
  str d0, [x0, #1456]
  .inst 0x1e76f000   // fmov d0, #imm8=0xb7
  str d0, [x0, #1464]
  .inst 0x1e771000   // fmov d0, #imm8=0xb8
  str d0, [x0, #1472]
  .inst 0x1e773000   // fmov d0, #imm8=0xb9
  str d0, [x0, #1480]
  .inst 0x1e775000   // fmov d0, #imm8=0xba
  str d0, [x0, #1488]
  .inst 0x1e777000   // fmov d0, #imm8=0xbb
  str d0, [x0, #1496]
  .inst 0x1e779000   // fmov d0, #imm8=0xbc
  str d0, [x0, #1504]
  .inst 0x1e77b000   // fmov d0, #imm8=0xbd
  str d0, [x0, #1512]
  .inst 0x1e77d000   // fmov d0, #imm8=0xbe
  str d0, [x0, #1520]
  .inst 0x1e77f000   // fmov d0, #imm8=0xbf
  str d0, [x0, #1528]
  .inst 0x1e781000   // fmov d0, #imm8=0xc0
  str d0, [x0, #1536]
  .inst 0x1e783000   // fmov d0, #imm8=0xc1
  str d0, [x0, #1544]
  .inst 0x1e785000   // fmov d0, #imm8=0xc2
  str d0, [x0, #1552]
  .inst 0x1e787000   // fmov d0, #imm8=0xc3
  str d0, [x0, #1560]
  .inst 0x1e789000   // fmov d0, #imm8=0xc4
  str d0, [x0, #1568]
  .inst 0x1e78b000   // fmov d0, #imm8=0xc5
  str d0, [x0, #1576]
  .inst 0x1e78d000   // fmov d0, #imm8=0xc6
  str d0, [x0, #1584]
  .inst 0x1e78f000   // fmov d0, #imm8=0xc7
  str d0, [x0, #1592]
  .inst 0x1e791000   // fmov d0, #imm8=0xc8
  str d0, [x0, #1600]
  .inst 0x1e793000   // fmov d0, #imm8=0xc9
  str d0, [x0, #1608]
  .inst 0x1e795000   // fmov d0, #imm8=0xca
  str d0, [x0, #1616]
  .inst 0x1e797000   // fmov d0, #imm8=0xcb
  str d0, [x0, #1624]
  .inst 0x1e799000   // fmov d0, #imm8=0xcc
  str d0, [x0, #1632]
  .inst 0x1e79b000   // fmov d0, #imm8=0xcd
  str d0, [x0, #1640]
  .inst 0x1e79d000   // fmov d0, #imm8=0xce
  str d0, [x0, #1648]
  .inst 0x1e79f000   // fmov d0, #imm8=0xcf
  str d0, [x0, #1656]
  .inst 0x1e7a1000   // fmov d0, #imm8=0xd0
  str d0, [x0, #1664]
  .inst 0x1e7a3000   // fmov d0, #imm8=0xd1
  str d0, [x0, #1672]
  .inst 0x1e7a5000   // fmov d0, #imm8=0xd2
  str d0, [x0, #1680]
  .inst 0x1e7a7000   // fmov d0, #imm8=0xd3
  str d0, [x0, #1688]
  .inst 0x1e7a9000   // fmov d0, #imm8=0xd4
  str d0, [x0, #1696]
  .inst 0x1e7ab000   // fmov d0, #imm8=0xd5
  str d0, [x0, #1704]
  .inst 0x1e7ad000   // fmov d0, #imm8=0xd6
  str d0, [x0, #1712]
  .inst 0x1e7af000   // fmov d0, #imm8=0xd7
  str d0, [x0, #1720]
  .inst 0x1e7b1000   // fmov d0, #imm8=0xd8
  str d0, [x0, #1728]
  .inst 0x1e7b3000   // fmov d0, #imm8=0xd9
  str d0, [x0, #1736]
  .inst 0x1e7b5000   // fmov d0, #imm8=0xda
  str d0, [x0, #1744]
  .inst 0x1e7b7000   // fmov d0, #imm8=0xdb
  str d0, [x0, #1752]
  .inst 0x1e7b9000   // fmov d0, #imm8=0xdc
  str d0, [x0, #1760]
  .inst 0x1e7bb000   // fmov d0, #imm8=0xdd
  str d0, [x0, #1768]
  .inst 0x1e7bd000   // fmov d0, #imm8=0xde
  str d0, [x0, #1776]
  .inst 0x1e7bf000   // fmov d0, #imm8=0xdf
  str d0, [x0, #1784]
  .inst 0x1e7c1000   // fmov d0, #imm8=0xe0
  str d0, [x0, #1792]
  .inst 0x1e7c3000   // fmov d0, #imm8=0xe1
  str d0, [x0, #1800]
  .inst 0x1e7c5000   // fmov d0, #imm8=0xe2
  str d0, [x0, #1808]
  .inst 0x1e7c7000   // fmov d0, #imm8=0xe3
  str d0, [x0, #1816]
  .inst 0x1e7c9000   // fmov d0, #imm8=0xe4
  str d0, [x0, #1824]
  .inst 0x1e7cb000   // fmov d0, #imm8=0xe5
  str d0, [x0, #1832]
  .inst 0x1e7cd000   // fmov d0, #imm8=0xe6
  str d0, [x0, #1840]
  .inst 0x1e7cf000   // fmov d0, #imm8=0xe7
  str d0, [x0, #1848]
  .inst 0x1e7d1000   // fmov d0, #imm8=0xe8
  str d0, [x0, #1856]
  .inst 0x1e7d3000   // fmov d0, #imm8=0xe9
  str d0, [x0, #1864]
  .inst 0x1e7d5000   // fmov d0, #imm8=0xea
  str d0, [x0, #1872]
  .inst 0x1e7d7000   // fmov d0, #imm8=0xeb
  str d0, [x0, #1880]
  .inst 0x1e7d9000   // fmov d0, #imm8=0xec
  str d0, [x0, #1888]
  .inst 0x1e7db000   // fmov d0, #imm8=0xed
  str d0, [x0, #1896]
  .inst 0x1e7dd000   // fmov d0, #imm8=0xee
  str d0, [x0, #1904]
  .inst 0x1e7df000   // fmov d0, #imm8=0xef
  str d0, [x0, #1912]
  .inst 0x1e7e1000   // fmov d0, #imm8=0xf0
  str d0, [x0, #1920]
  .inst 0x1e7e3000   // fmov d0, #imm8=0xf1
  str d0, [x0, #1928]
  .inst 0x1e7e5000   // fmov d0, #imm8=0xf2
  str d0, [x0, #1936]
  .inst 0x1e7e7000   // fmov d0, #imm8=0xf3
  str d0, [x0, #1944]
  .inst 0x1e7e9000   // fmov d0, #imm8=0xf4
  str d0, [x0, #1952]
  .inst 0x1e7eb000   // fmov d0, #imm8=0xf5
  str d0, [x0, #1960]
  .inst 0x1e7ed000   // fmov d0, #imm8=0xf6
  str d0, [x0, #1968]
  .inst 0x1e7ef000   // fmov d0, #imm8=0xf7
  str d0, [x0, #1976]
  .inst 0x1e7f1000   // fmov d0, #imm8=0xf8
  str d0, [x0, #1984]
  .inst 0x1e7f3000   // fmov d0, #imm8=0xf9
  str d0, [x0, #1992]
  .inst 0x1e7f5000   // fmov d0, #imm8=0xfa
  str d0, [x0, #2000]
  .inst 0x1e7f7000   // fmov d0, #imm8=0xfb
  str d0, [x0, #2008]
  .inst 0x1e7f9000   // fmov d0, #imm8=0xfc
  str d0, [x0, #2016]
  .inst 0x1e7fb000   // fmov d0, #imm8=0xfd
  str d0, [x0, #2024]
  .inst 0x1e7fd000   // fmov d0, #imm8=0xfe
  str d0, [x0, #2032]
  .inst 0x1e7ff000   // fmov d0, #imm8=0xff
  str d0, [x0, #2040]
  ret

.global NAME(fmov_imm_s_table)
.align 4
NAME(fmov_imm_s_table):
  .inst 0x1e201000   // fmov s0, #imm8=0x00
  str s0, [x0, #0]
  .inst 0x1e203000   // fmov s0, #imm8=0x01
  str s0, [x0, #4]
  .inst 0x1e205000   // fmov s0, #imm8=0x02
  str s0, [x0, #8]
  .inst 0x1e207000   // fmov s0, #imm8=0x03
  str s0, [x0, #12]
  .inst 0x1e209000   // fmov s0, #imm8=0x04
  str s0, [x0, #16]
  .inst 0x1e20b000   // fmov s0, #imm8=0x05
  str s0, [x0, #20]
  .inst 0x1e20d000   // fmov s0, #imm8=0x06
  str s0, [x0, #24]
  .inst 0x1e20f000   // fmov s0, #imm8=0x07
  str s0, [x0, #28]
  .inst 0x1e211000   // fmov s0, #imm8=0x08
  str s0, [x0, #32]
  .inst 0x1e213000   // fmov s0, #imm8=0x09
  str s0, [x0, #36]
  .inst 0x1e215000   // fmov s0, #imm8=0x0a
  str s0, [x0, #40]
  .inst 0x1e217000   // fmov s0, #imm8=0x0b
  str s0, [x0, #44]
  .inst 0x1e219000   // fmov s0, #imm8=0x0c
  str s0, [x0, #48]
  .inst 0x1e21b000   // fmov s0, #imm8=0x0d
  str s0, [x0, #52]
  .inst 0x1e21d000   // fmov s0, #imm8=0x0e
  str s0, [x0, #56]
  .inst 0x1e21f000   // fmov s0, #imm8=0x0f
  str s0, [x0, #60]
  .inst 0x1e221000   // fmov s0, #imm8=0x10
  str s0, [x0, #64]
  .inst 0x1e223000   // fmov s0, #imm8=0x11
  str s0, [x0, #68]
  .inst 0x1e225000   // fmov s0, #imm8=0x12
  str s0, [x0, #72]
  .inst 0x1e227000   // fmov s0, #imm8=0x13
  str s0, [x0, #76]
  .inst 0x1e229000   // fmov s0, #imm8=0x14
  str s0, [x0, #80]
  .inst 0x1e22b000   // fmov s0, #imm8=0x15
  str s0, [x0, #84]
  .inst 0x1e22d000   // fmov s0, #imm8=0x16
  str s0, [x0, #88]
  .inst 0x1e22f000   // fmov s0, #imm8=0x17
  str s0, [x0, #92]
  .inst 0x1e231000   // fmov s0, #imm8=0x18
  str s0, [x0, #96]
  .inst 0x1e233000   // fmov s0, #imm8=0x19
  str s0, [x0, #100]
  .inst 0x1e235000   // fmov s0, #imm8=0x1a
  str s0, [x0, #104]
  .inst 0x1e237000   // fmov s0, #imm8=0x1b
  str s0, [x0, #108]
  .inst 0x1e239000   // fmov s0, #imm8=0x1c
  str s0, [x0, #112]
  .inst 0x1e23b000   // fmov s0, #imm8=0x1d
  str s0, [x0, #116]
  .inst 0x1e23d000   // fmov s0, #imm8=0x1e
  str s0, [x0, #120]
  .inst 0x1e23f000   // fmov s0, #imm8=0x1f
  str s0, [x0, #124]
  .inst 0x1e241000   // fmov s0, #imm8=0x20
  str s0, [x0, #128]
  .inst 0x1e243000   // fmov s0, #imm8=0x21
  str s0, [x0, #132]
  .inst 0x1e245000   // fmov s0, #imm8=0x22
  str s0, [x0, #136]
  .inst 0x1e247000   // fmov s0, #imm8=0x23
  str s0, [x0, #140]
  .inst 0x1e249000   // fmov s0, #imm8=0x24
  str s0, [x0, #144]
  .inst 0x1e24b000   // fmov s0, #imm8=0x25
  str s0, [x0, #148]
  .inst 0x1e24d000   // fmov s0, #imm8=0x26
  str s0, [x0, #152]
  .inst 0x1e24f000   // fmov s0, #imm8=0x27
  str s0, [x0, #156]
  .inst 0x1e251000   // fmov s0, #imm8=0x28
  str s0, [x0, #160]
  .inst 0x1e253000   // fmov s0, #imm8=0x29
  str s0, [x0, #164]
  .inst 0x1e255000   // fmov s0, #imm8=0x2a
  str s0, [x0, #168]
  .inst 0x1e257000   // fmov s0, #imm8=0x2b
  str s0, [x0, #172]
  .inst 0x1e259000   // fmov s0, #imm8=0x2c
  str s0, [x0, #176]
  .inst 0x1e25b000   // fmov s0, #imm8=0x2d
  str s0, [x0, #180]
  .inst 0x1e25d000   // fmov s0, #imm8=0x2e
  str s0, [x0, #184]
  .inst 0x1e25f000   // fmov s0, #imm8=0x2f
  str s0, [x0, #188]
  .inst 0x1e261000   // fmov s0, #imm8=0x30
  str s0, [x0, #192]
  .inst 0x1e263000   // fmov s0, #imm8=0x31
  str s0, [x0, #196]
  .inst 0x1e265000   // fmov s0, #imm8=0x32
  str s0, [x0, #200]
  .inst 0x1e267000   // fmov s0, #imm8=0x33
  str s0, [x0, #204]
  .inst 0x1e269000   // fmov s0, #imm8=0x34
  str s0, [x0, #208]
  .inst 0x1e26b000   // fmov s0, #imm8=0x35
  str s0, [x0, #212]
  .inst 0x1e26d000   // fmov s0, #imm8=0x36
  str s0, [x0, #216]
  .inst 0x1e26f000   // fmov s0, #imm8=0x37
  str s0, [x0, #220]
  .inst 0x1e271000   // fmov s0, #imm8=0x38
  str s0, [x0, #224]
  .inst 0x1e273000   // fmov s0, #imm8=0x39
  str s0, [x0, #228]
  .inst 0x1e275000   // fmov s0, #imm8=0x3a
  str s0, [x0, #232]
  .inst 0x1e277000   // fmov s0, #imm8=0x3b
  str s0, [x0, #236]
  .inst 0x1e279000   // fmov s0, #imm8=0x3c
  str s0, [x0, #240]
  .inst 0x1e27b000   // fmov s0, #imm8=0x3d
  str s0, [x0, #244]
  .inst 0x1e27d000   // fmov s0, #imm8=0x3e
  str s0, [x0, #248]
  .inst 0x1e27f000   // fmov s0, #imm8=0x3f
  str s0, [x0, #252]
  .inst 0x1e281000   // fmov s0, #imm8=0x40
  str s0, [x0, #256]
  .inst 0x1e283000   // fmov s0, #imm8=0x41
  str s0, [x0, #260]
  .inst 0x1e285000   // fmov s0, #imm8=0x42
  str s0, [x0, #264]
  .inst 0x1e287000   // fmov s0, #imm8=0x43
  str s0, [x0, #268]
  .inst 0x1e289000   // fmov s0, #imm8=0x44
  str s0, [x0, #272]
  .inst 0x1e28b000   // fmov s0, #imm8=0x45
  str s0, [x0, #276]
  .inst 0x1e28d000   // fmov s0, #imm8=0x46
  str s0, [x0, #280]
  .inst 0x1e28f000   // fmov s0, #imm8=0x47
  str s0, [x0, #284]
  .inst 0x1e291000   // fmov s0, #imm8=0x48
  str s0, [x0, #288]
  .inst 0x1e293000   // fmov s0, #imm8=0x49
  str s0, [x0, #292]
  .inst 0x1e295000   // fmov s0, #imm8=0x4a
  str s0, [x0, #296]
  .inst 0x1e297000   // fmov s0, #imm8=0x4b
  str s0, [x0, #300]
  .inst 0x1e299000   // fmov s0, #imm8=0x4c
  str s0, [x0, #304]
  .inst 0x1e29b000   // fmov s0, #imm8=0x4d
  str s0, [x0, #308]
  .inst 0x1e29d000   // fmov s0, #imm8=0x4e
  str s0, [x0, #312]
  .inst 0x1e29f000   // fmov s0, #imm8=0x4f
  str s0, [x0, #316]
  .inst 0x1e2a1000   // fmov s0, #imm8=0x50
  str s0, [x0, #320]
  .inst 0x1e2a3000   // fmov s0, #imm8=0x51
  str s0, [x0, #324]
  .inst 0x1e2a5000   // fmov s0, #imm8=0x52
  str s0, [x0, #328]
  .inst 0x1e2a7000   // fmov s0, #imm8=0x53
  str s0, [x0, #332]
  .inst 0x1e2a9000   // fmov s0, #imm8=0x54
  str s0, [x0, #336]
  .inst 0x1e2ab000   // fmov s0, #imm8=0x55
  str s0, [x0, #340]
  .inst 0x1e2ad000   // fmov s0, #imm8=0x56
  str s0, [x0, #344]
  .inst 0x1e2af000   // fmov s0, #imm8=0x57
  str s0, [x0, #348]
  .inst 0x1e2b1000   // fmov s0, #imm8=0x58
  str s0, [x0, #352]
  .inst 0x1e2b3000   // fmov s0, #imm8=0x59
  str s0, [x0, #356]
  .inst 0x1e2b5000   // fmov s0, #imm8=0x5a
  str s0, [x0, #360]
  .inst 0x1e2b7000   // fmov s0, #imm8=0x5b
  str s0, [x0, #364]
  .inst 0x1e2b9000   // fmov s0, #imm8=0x5c
  str s0, [x0, #368]
  .inst 0x1e2bb000   // fmov s0, #imm8=0x5d
  str s0, [x0, #372]
  .inst 0x1e2bd000   // fmov s0, #imm8=0x5e
  str s0, [x0, #376]
  .inst 0x1e2bf000   // fmov s0, #imm8=0x5f
  str s0, [x0, #380]
  .inst 0x1e2c1000   // fmov s0, #imm8=0x60
  str s0, [x0, #384]
  .inst 0x1e2c3000   // fmov s0, #imm8=0x61
  str s0, [x0, #388]
  .inst 0x1e2c5000   // fmov s0, #imm8=0x62
  str s0, [x0, #392]
  .inst 0x1e2c7000   // fmov s0, #imm8=0x63
  str s0, [x0, #396]
  .inst 0x1e2c9000   // fmov s0, #imm8=0x64
  str s0, [x0, #400]
  .inst 0x1e2cb000   // fmov s0, #imm8=0x65
  str s0, [x0, #404]
  .inst 0x1e2cd000   // fmov s0, #imm8=0x66
  str s0, [x0, #408]
  .inst 0x1e2cf000   // fmov s0, #imm8=0x67
  str s0, [x0, #412]
  .inst 0x1e2d1000   // fmov s0, #imm8=0x68
  str s0, [x0, #416]
  .inst 0x1e2d3000   // fmov s0, #imm8=0x69
  str s0, [x0, #420]
  .inst 0x1e2d5000   // fmov s0, #imm8=0x6a
  str s0, [x0, #424]
  .inst 0x1e2d7000   // fmov s0, #imm8=0x6b
  str s0, [x0, #428]
  .inst 0x1e2d9000   // fmov s0, #imm8=0x6c
  str s0, [x0, #432]
  .inst 0x1e2db000   // fmov s0, #imm8=0x6d
  str s0, [x0, #436]
  .inst 0x1e2dd000   // fmov s0, #imm8=0x6e
  str s0, [x0, #440]
  .inst 0x1e2df000   // fmov s0, #imm8=0x6f
  str s0, [x0, #444]
  .inst 0x1e2e1000   // fmov s0, #imm8=0x70
  str s0, [x0, #448]
  .inst 0x1e2e3000   // fmov s0, #imm8=0x71
  str s0, [x0, #452]
  .inst 0x1e2e5000   // fmov s0, #imm8=0x72
  str s0, [x0, #456]
  .inst 0x1e2e7000   // fmov s0, #imm8=0x73
  str s0, [x0, #460]
  .inst 0x1e2e9000   // fmov s0, #imm8=0x74
  str s0, [x0, #464]
  .inst 0x1e2eb000   // fmov s0, #imm8=0x75
  str s0, [x0, #468]
  .inst 0x1e2ed000   // fmov s0, #imm8=0x76
  str s0, [x0, #472]
  .inst 0x1e2ef000   // fmov s0, #imm8=0x77
  str s0, [x0, #476]
  .inst 0x1e2f1000   // fmov s0, #imm8=0x78
  str s0, [x0, #480]
  .inst 0x1e2f3000   // fmov s0, #imm8=0x79
  str s0, [x0, #484]
  .inst 0x1e2f5000   // fmov s0, #imm8=0x7a
  str s0, [x0, #488]
  .inst 0x1e2f7000   // fmov s0, #imm8=0x7b
  str s0, [x0, #492]
  .inst 0x1e2f9000   // fmov s0, #imm8=0x7c
  str s0, [x0, #496]
  .inst 0x1e2fb000   // fmov s0, #imm8=0x7d
  str s0, [x0, #500]
  .inst 0x1e2fd000   // fmov s0, #imm8=0x7e
  str s0, [x0, #504]
  .inst 0x1e2ff000   // fmov s0, #imm8=0x7f
  str s0, [x0, #508]
  .inst 0x1e301000   // fmov s0, #imm8=0x80
  str s0, [x0, #512]
  .inst 0x1e303000   // fmov s0, #imm8=0x81
  str s0, [x0, #516]
  .inst 0x1e305000   // fmov s0, #imm8=0x82
  str s0, [x0, #520]
  .inst 0x1e307000   // fmov s0, #imm8=0x83
  str s0, [x0, #524]
  .inst 0x1e309000   // fmov s0, #imm8=0x84
  str s0, [x0, #528]
  .inst 0x1e30b000   // fmov s0, #imm8=0x85
  str s0, [x0, #532]
  .inst 0x1e30d000   // fmov s0, #imm8=0x86
  str s0, [x0, #536]
  .inst 0x1e30f000   // fmov s0, #imm8=0x87
  str s0, [x0, #540]
  .inst 0x1e311000   // fmov s0, #imm8=0x88
  str s0, [x0, #544]
  .inst 0x1e313000   // fmov s0, #imm8=0x89
  str s0, [x0, #548]
  .inst 0x1e315000   // fmov s0, #imm8=0x8a
  str s0, [x0, #552]
  .inst 0x1e317000   // fmov s0, #imm8=0x8b
  str s0, [x0, #556]
  .inst 0x1e319000   // fmov s0, #imm8=0x8c
  str s0, [x0, #560]
  .inst 0x1e31b000   // fmov s0, #imm8=0x8d
  str s0, [x0, #564]
  .inst 0x1e31d000   // fmov s0, #imm8=0x8e
  str s0, [x0, #568]
  .inst 0x1e31f000   // fmov s0, #imm8=0x8f
  str s0, [x0, #572]
  .inst 0x1e321000   // fmov s0, #imm8=0x90
  str s0, [x0, #576]
  .inst 0x1e323000   // fmov s0, #imm8=0x91
  str s0, [x0, #580]
  .inst 0x1e325000   // fmov s0, #imm8=0x92
  str s0, [x0, #584]
  .inst 0x1e327000   // fmov s0, #imm8=0x93
  str s0, [x0, #588]
  .inst 0x1e329000   // fmov s0, #imm8=0x94
  str s0, [x0, #592]
  .inst 0x1e32b000   // fmov s0, #imm8=0x95
  str s0, [x0, #596]
  .inst 0x1e32d000   // fmov s0, #imm8=0x96
  str s0, [x0, #600]
  .inst 0x1e32f000   // fmov s0, #imm8=0x97
  str s0, [x0, #604]
  .inst 0x1e331000   // fmov s0, #imm8=0x98
  str s0, [x0, #608]
  .inst 0x1e333000   // fmov s0, #imm8=0x99
  str s0, [x0, #612]
  .inst 0x1e335000   // fmov s0, #imm8=0x9a
  str s0, [x0, #616]
  .inst 0x1e337000   // fmov s0, #imm8=0x9b
  str s0, [x0, #620]
  .inst 0x1e339000   // fmov s0, #imm8=0x9c
  str s0, [x0, #624]
  .inst 0x1e33b000   // fmov s0, #imm8=0x9d
  str s0, [x0, #628]
  .inst 0x1e33d000   // fmov s0, #imm8=0x9e
  str s0, [x0, #632]
  .inst 0x1e33f000   // fmov s0, #imm8=0x9f
  str s0, [x0, #636]
  .inst 0x1e341000   // fmov s0, #imm8=0xa0
  str s0, [x0, #640]
  .inst 0x1e343000   // fmov s0, #imm8=0xa1
  str s0, [x0, #644]
  .inst 0x1e345000   // fmov s0, #imm8=0xa2
  str s0, [x0, #648]
  .inst 0x1e347000   // fmov s0, #imm8=0xa3
  str s0, [x0, #652]
  .inst 0x1e349000   // fmov s0, #imm8=0xa4
  str s0, [x0, #656]
  .inst 0x1e34b000   // fmov s0, #imm8=0xa5
  str s0, [x0, #660]
  .inst 0x1e34d000   // fmov s0, #imm8=0xa6
  str s0, [x0, #664]
  .inst 0x1e34f000   // fmov s0, #imm8=0xa7
  str s0, [x0, #668]
  .inst 0x1e351000   // fmov s0, #imm8=0xa8
  str s0, [x0, #672]
  .inst 0x1e353000   // fmov s0, #imm8=0xa9
  str s0, [x0, #676]
  .inst 0x1e355000   // fmov s0, #imm8=0xaa
  str s0, [x0, #680]
  .inst 0x1e357000   // fmov s0, #imm8=0xab
  str s0, [x0, #684]
  .inst 0x1e359000   // fmov s0, #imm8=0xac
  str s0, [x0, #688]
  .inst 0x1e35b000   // fmov s0, #imm8=0xad
  str s0, [x0, #692]
  .inst 0x1e35d000   // fmov s0, #imm8=0xae
  str s0, [x0, #696]
  .inst 0x1e35f000   // fmov s0, #imm8=0xaf
  str s0, [x0, #700]
  .inst 0x1e361000   // fmov s0, #imm8=0xb0
  str s0, [x0, #704]
  .inst 0x1e363000   // fmov s0, #imm8=0xb1
  str s0, [x0, #708]
  .inst 0x1e365000   // fmov s0, #imm8=0xb2
  str s0, [x0, #712]
  .inst 0x1e367000   // fmov s0, #imm8=0xb3
  str s0, [x0, #716]
  .inst 0x1e369000   // fmov s0, #imm8=0xb4
  str s0, [x0, #720]
  .inst 0x1e36b000   // fmov s0, #imm8=0xb5
  str s0, [x0, #724]
  .inst 0x1e36d000   // fmov s0, #imm8=0xb6
  str s0, [x0, #728]
  .inst 0x1e36f000   // fmov s0, #imm8=0xb7
  str s0, [x0, #732]
  .inst 0x1e371000   // fmov s0, #imm8=0xb8
  str s0, [x0, #736]
  .inst 0x1e373000   // fmov s0, #imm8=0xb9
  str s0, [x0, #740]
  .inst 0x1e375000   // fmov s0, #imm8=0xba
  str s0, [x0, #744]
  .inst 0x1e377000   // fmov s0, #imm8=0xbb
  str s0, [x0, #748]
  .inst 0x1e379000   // fmov s0, #imm8=0xbc
  str s0, [x0, #752]
  .inst 0x1e37b000   // fmov s0, #imm8=0xbd
  str s0, [x0, #756]
  .inst 0x1e37d000   // fmov s0, #imm8=0xbe
  str s0, [x0, #760]
  .inst 0x1e37f000   // fmov s0, #imm8=0xbf
  str s0, [x0, #764]
  .inst 0x1e381000   // fmov s0, #imm8=0xc0
  str s0, [x0, #768]
  .inst 0x1e383000   // fmov s0, #imm8=0xc1
  str s0, [x0, #772]
  .inst 0x1e385000   // fmov s0, #imm8=0xc2
  str s0, [x0, #776]
  .inst 0x1e387000   // fmov s0, #imm8=0xc3
  str s0, [x0, #780]
  .inst 0x1e389000   // fmov s0, #imm8=0xc4
  str s0, [x0, #784]
  .inst 0x1e38b000   // fmov s0, #imm8=0xc5
  str s0, [x0, #788]
  .inst 0x1e38d000   // fmov s0, #imm8=0xc6
  str s0, [x0, #792]
  .inst 0x1e38f000   // fmov s0, #imm8=0xc7
  str s0, [x0, #796]
  .inst 0x1e391000   // fmov s0, #imm8=0xc8
  str s0, [x0, #800]
  .inst 0x1e393000   // fmov s0, #imm8=0xc9
  str s0, [x0, #804]
  .inst 0x1e395000   // fmov s0, #imm8=0xca
  str s0, [x0, #808]
  .inst 0x1e397000   // fmov s0, #imm8=0xcb
  str s0, [x0, #812]
  .inst 0x1e399000   // fmov s0, #imm8=0xcc
  str s0, [x0, #816]
  .inst 0x1e39b000   // fmov s0, #imm8=0xcd
  str s0, [x0, #820]
  .inst 0x1e39d000   // fmov s0, #imm8=0xce
  str s0, [x0, #824]
  .inst 0x1e39f000   // fmov s0, #imm8=0xcf
  str s0, [x0, #828]
  .inst 0x1e3a1000   // fmov s0, #imm8=0xd0
  str s0, [x0, #832]
  .inst 0x1e3a3000   // fmov s0, #imm8=0xd1
  str s0, [x0, #836]
  .inst 0x1e3a5000   // fmov s0, #imm8=0xd2
  str s0, [x0, #840]
  .inst 0x1e3a7000   // fmov s0, #imm8=0xd3
  str s0, [x0, #844]
  .inst 0x1e3a9000   // fmov s0, #imm8=0xd4
  str s0, [x0, #848]
  .inst 0x1e3ab000   // fmov s0, #imm8=0xd5
  str s0, [x0, #852]
  .inst 0x1e3ad000   // fmov s0, #imm8=0xd6
  str s0, [x0, #856]
  .inst 0x1e3af000   // fmov s0, #imm8=0xd7
  str s0, [x0, #860]
  .inst 0x1e3b1000   // fmov s0, #imm8=0xd8
  str s0, [x0, #864]
  .inst 0x1e3b3000   // fmov s0, #imm8=0xd9
  str s0, [x0, #868]
  .inst 0x1e3b5000   // fmov s0, #imm8=0xda
  str s0, [x0, #872]
  .inst 0x1e3b7000   // fmov s0, #imm8=0xdb
  str s0, [x0, #876]
  .inst 0x1e3b9000   // fmov s0, #imm8=0xdc
  str s0, [x0, #880]
  .inst 0x1e3bb000   // fmov s0, #imm8=0xdd
  str s0, [x0, #884]
  .inst 0x1e3bd000   // fmov s0, #imm8=0xde
  str s0, [x0, #888]
  .inst 0x1e3bf000   // fmov s0, #imm8=0xdf
  str s0, [x0, #892]
  .inst 0x1e3c1000   // fmov s0, #imm8=0xe0
  str s0, [x0, #896]
  .inst 0x1e3c3000   // fmov s0, #imm8=0xe1
  str s0, [x0, #900]
  .inst 0x1e3c5000   // fmov s0, #imm8=0xe2
  str s0, [x0, #904]
  .inst 0x1e3c7000   // fmov s0, #imm8=0xe3
  str s0, [x0, #908]
  .inst 0x1e3c9000   // fmov s0, #imm8=0xe4
  str s0, [x0, #912]
  .inst 0x1e3cb000   // fmov s0, #imm8=0xe5
  str s0, [x0, #916]
  .inst 0x1e3cd000   // fmov s0, #imm8=0xe6
  str s0, [x0, #920]
  .inst 0x1e3cf000   // fmov s0, #imm8=0xe7
  str s0, [x0, #924]
  .inst 0x1e3d1000   // fmov s0, #imm8=0xe8
  str s0, [x0, #928]
  .inst 0x1e3d3000   // fmov s0, #imm8=0xe9
  str s0, [x0, #932]
  .inst 0x1e3d5000   // fmov s0, #imm8=0xea
  str s0, [x0, #936]
  .inst 0x1e3d7000   // fmov s0, #imm8=0xeb
  str s0, [x0, #940]
  .inst 0x1e3d9000   // fmov s0, #imm8=0xec
  str s0, [x0, #944]
  .inst 0x1e3db000   // fmov s0, #imm8=0xed
  str s0, [x0, #948]
  .inst 0x1e3dd000   // fmov s0, #imm8=0xee
  str s0, [x0, #952]
  .inst 0x1e3df000   // fmov s0, #imm8=0xef
  str s0, [x0, #956]
  .inst 0x1e3e1000   // fmov s0, #imm8=0xf0
  str s0, [x0, #960]
  .inst 0x1e3e3000   // fmov s0, #imm8=0xf1
  str s0, [x0, #964]
  .inst 0x1e3e5000   // fmov s0, #imm8=0xf2
  str s0, [x0, #968]
  .inst 0x1e3e7000   // fmov s0, #imm8=0xf3
  str s0, [x0, #972]
  .inst 0x1e3e9000   // fmov s0, #imm8=0xf4
  str s0, [x0, #976]
  .inst 0x1e3eb000   // fmov s0, #imm8=0xf5
  str s0, [x0, #980]
  .inst 0x1e3ed000   // fmov s0, #imm8=0xf6
  str s0, [x0, #984]
  .inst 0x1e3ef000   // fmov s0, #imm8=0xf7
  str s0, [x0, #988]
  .inst 0x1e3f1000   // fmov s0, #imm8=0xf8
  str s0, [x0, #992]
  .inst 0x1e3f3000   // fmov s0, #imm8=0xf9
  str s0, [x0, #996]
  .inst 0x1e3f5000   // fmov s0, #imm8=0xfa
  str s0, [x0, #1000]
  .inst 0x1e3f7000   // fmov s0, #imm8=0xfb
  str s0, [x0, #1004]
  .inst 0x1e3f9000   // fmov s0, #imm8=0xfc
  str s0, [x0, #1008]
  .inst 0x1e3fb000   // fmov s0, #imm8=0xfd
  str s0, [x0, #1012]
  .inst 0x1e3fd000   // fmov s0, #imm8=0xfe
  str s0, [x0, #1016]
  .inst 0x1e3ff000   // fmov s0, #imm8=0xff
  str s0, [x0, #1020]
  ret
