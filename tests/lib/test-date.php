<?php
// EXPECT: ymd=1970-01-01
// EXPECT: his=00:00:00
// EXPECT: ampm=AM
// EXPECT: full=Thursday, January 1, 1970
// EXPECT: mk=0
// EXPECT: cd_leap=true
// EXPECT: cd_norm=false
// EXPECT: cd_bad=false
// EXPECT: U0=0
// EXPECT: ok

// Unix epoch 0 = 1970-01-01 00:00:00 UTC
echo "ymd=", date("Y-m-d", 0), "\n";
echo "his=", date("H:i:s", 0), "\n";
echo "ampm=", date("A", 0), "\n";
echo "full=", date("l, F j, Y", 0), "\n";

// mktime roundtrip: 1970-01-01 00:00:00 UTC
echo "mk=", mktime(0, 0, 0, 1, 1, 1970), "\n";

echo "cd_leap=", checkdate(2, 29, 2024) ? "true" : "false", "\n";
echo "cd_norm=", checkdate(2, 29, 2023) ? "true" : "false", "\n";
echo "cd_bad=", checkdate(13, 1, 2023) ? "true" : "false", "\n";

echo "U0=", date("U", 0), "\n";
echo "ok\n";
