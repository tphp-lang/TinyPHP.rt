<?php
// 接口：常量、多实现、instanceof、接口继承
// EXPECT: VERSION: base-v1
// EXPECT: dog speaks: woof
// EXPECT: robot: beep + tick
// EXPECT: isa-Speakable: yes
// EXPECT: isa-Runnable: yes
// EXPECT: iface-const: base-v1

interface Speaker {
    const LOUD = "base-v1";
    public function speak(): string;
}

interface Runner {
    public function run(): string;
}

interface SpeakerRunner extends Speaker, Runner {
}

class Dog implements Speaker {
    public function speak(): string { return "woof"; }
}

class Robot implements SpeakerRunner {
    public function speak(): string { return "beep"; }
    public function run(): string { return "tick"; }
}

echo "VERSION: ", Speaker::LOUD, "\n";
$d = new Dog();
echo "dog speaks: ", $d->speak(), "\n";
$rb = new Robot();
echo "robot: ", $rb->speak(), " + ", $rb->run(), "\n";
echo "isa-Speakable: ", $d instanceof Speaker ? "yes" : "no", "\n";
echo "isa-Runnable: ", $rb instanceof Runner ? "yes" : "no", "\n";
echo "iface-const: ", SpeakerRunner::LOUD, "\n";
