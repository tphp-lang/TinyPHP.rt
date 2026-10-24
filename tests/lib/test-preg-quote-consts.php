<?php
// EXPECT: quoted=a\.b\+c
// EXPECT: const-offset=256
// EXPECT: ok
// P4: preg_quote + PREG_* 常量
echo "quoted=", preg_quote("a.b+c"), "\n";
echo "const-offset=", PREG_OFFSET_CAPTURE, "\n";
echo "ok\n";
