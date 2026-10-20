<?php
// EXPECT: a1
// EXPECT: plain_inc_loaded
// EXPECT: a2
// EXPECT: plain_inc_loaded
// EXPECT: ok

echo "a1\n";
require "helpers/plain_marker.php";
echo "a2\n";
require "helpers/plain_marker.php";   // 普通 require 可重复展开
echo "ok\n";
