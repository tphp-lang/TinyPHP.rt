<?php
// lib 工具函数（被 require 编译期内联）
function lib_double($x) {
    return $x * 2;
}

function lib_greet($name) {
    return "Hello, $name!";
}

const LIB_VERSION = "1.0";
