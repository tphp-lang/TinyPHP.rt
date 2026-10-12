<?php
// M4-e: http_get —— Content-Length、chunked、302 重定向（相对 Location）、
// 自定义头（opts['headers']）；自建 HTTP server 驱动
use function Async\spawn;
use function Async\await;

$srv = stream_socket_server("tcp://127.0.0.1:18301");

$server = spawn(function () use ($srv) {
    $c1 = stream_socket_accept($srv);
    fgets($c1);
    while (trim(fgets($c1)) !== "") {}
    fwrite($c1, "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nhello-http!!");
    fclose($c1);

    $c2 = stream_socket_accept($srv);
    fgets($c2);
    while (trim(fgets($c2)) !== "") {}
    fwrite($c2, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        . "5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n");
    fclose($c2);

    $c3 = stream_socket_accept($srv);
    fgets($c3);
    while (trim(fgets($c3)) !== "") {}
    fwrite($c3, "HTTP/1.1 302 Found\r\nLocation: /ok\r\n\r\n");
    fclose($c3);
    $c3b = stream_socket_accept($srv);
    fgets($c3b);
    while (trim(fgets($c3b)) !== "") {}
    fwrite($c3b, "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nredirect");
    fclose($c3b);

    $c4 = stream_socket_accept($srv);
    fgets($c4);
    $h4 = [];
    while (($l = trim(fgets($c4))) !== "") $h4[] = strtolower($l);
    $found = in_array("x-token: abc123", $h4);
    fwrite($c4, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n"
        . ($found ? "Y" : "N"));
    fclose($c4);
    fclose($srv);
    return "done";
});

var_dump(http_get("http://127.0.0.1:18301/ok"));
var_dump(http_get("http://127.0.0.1:18301/chunked"));
var_dump(http_get("http://127.0.0.1:18301/redir"));
var_dump(http_get("http://127.0.0.1:18301/hdr",
                  ["headers" => ["X-Token: abc123"]]));
var_dump(await($server));

// 失败路径：无人监听 → warning + false
var_dump(http_get("http://127.0.0.1:1/x") === false);

echo "http-done\n";

// EXPECT: string(12) "hello-http!!"
// EXPECT: string(12) "hello, world"
// EXPECT: string(8) "redirect"
// EXPECT: string(1) "Y"
// EXPECT: string(4) "done"
// EXPECT: bool(true)
// EXPECT: http-done
