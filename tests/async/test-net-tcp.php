<?php
// M4-d: DNS + TCP —— gethostbyname、fsockopen、stream_socket_server/accept、
// fgets/fread/fwrite/fclose 对 TCP 流、双协程挂起交互
use function Tphp\spawn;
use function Tphp\await;

// 1) DNS
var_dump(gethostbyname("localhost") === "127.0.0.1" ||
         strpos(gethostbyname("localhost"), ".") > 0);
$ips = gethostbynamel("localhost");
var_dump(is_array($ips) && count($ips) >= 1);

// 2) TCP echo 全链路（固定高位端口；测试间不并行，无冲突）
$srv = stream_socket_server("tcp://127.0.0.1:18234");
var_dump($srv instanceof TStream);

$client = spawn(function () {
    $tcp = fsockopen("127.0.0.1", 18234);
    if ($tcp === false) return "connect-failed";
    fwrite($tcp, "line-one\n");
    $echo1 = fgets($tcp);              // server 回显（含 \n）
    fwrite($tcp, "rawchunk");
    $echo2 = fread($tcp, 64);          // server 原样回
    fwrite($tcp, "fin\n");
    $echo3 = fgets($tcp);
    fclose($tcp);
    return $echo1 . "+" . $echo2 . "+" . trim($echo3);
});

$server = spawn(function () use ($srv) {
    $conn = stream_socket_accept($srv);   // 挂起等连接
    $l1 = fgets($conn);                   // line-one\n
    fwrite($conn, $l1);                   // 回显
    $c2 = fread($conn, 64);               // rawchunk（无换行，读到可用块）
    fwrite($conn, $c2);
    $l3 = fgets($conn);                   // fin\n
    fwrite($conn, $l3);                   // 回显
    fclose($conn);
    fclose($srv);
    return "server-ok";
});

var_dump(await($client));
var_dump(await($server));

// 3) 连接失败路径：没人监听的端口 → warning + false
var_dump(fsockopen("127.0.0.1", 1) === false);

echo "net-done\n";

// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: string(22) "line-one
// EXPECT: +rawchunk+fin"
// EXPECT: string(9) "server-ok"
// EXPECT: bool(true)
// EXPECT: net-done
