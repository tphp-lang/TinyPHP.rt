<?php
// EXPECT: null
// EXPECT: true
// EXPECT: false
// EXPECT: 42
// EXPECT: -7
// EXPECT: 3.5
// EXPECT: "hello"
// EXPECT: "a\nb"
// EXPECT: [1,2,3]
// EXPECT: {"a":1,"b":2}
// EXPECT: []
// EXPECT: {"0":1,"k":2,"1":3}
// EXPECT: ok

echo json_encode(null), "\n";
echo json_encode(true), "\n";
echo json_encode(false), "\n";
echo json_encode(42), "\n";
echo json_encode(-7), "\n";
echo json_encode(3.5), "\n";
echo json_encode("hello"), "\n";
echo json_encode("a\nb"), "\n";
echo json_encode([1, 2, 3]), "\n";
echo json_encode(["a" => 1, "b" => 2]), "\n";
echo json_encode([]), "\n";
echo json_encode([1, "k" => 2, 3]), "\n";
echo "ok\n";
