<?php
// PLAN/01 §11 OOP 完整版验收示例（M3 结束应全部可跑）
// Animal::describe 里 static::class 按调用类（Animal）晚期绑定，输出 Animal
// EXPECT: App\Model\Animal [Rex] says woof
// EXPECT: Hello, woof
// EXPECT: status=A
// EXPECT: App\Model\Status

namespace App\Model;

interface Speakable {
    const VERSION = 1;
    public function speak(): string;
}

trait Greet {
    public function hello(): string {
        return "Hello, {$this->speak()}";
    }
}

enum Status: string implements Speakable {
    case Active = 'A';
    case Closed = 'C';

    public function speak(): string {
        return "status={$this->value}";
    }
    public static function fromLabel(string $s): static {
        return match($s) { 'on' => self::Active, default => self::Closed };
    }
}

abstract class Animal implements Speakable {
    use Greet;

    public function __construct(protected readonly string $name) {}

    abstract public function speak(): string;

    public static function describe(Animal $a): string {
        return static::class . " [{$a->name}] says " . $a->speak();
    }
}

final class Dog extends Animal {
    public function speak(): string { return 'woof'; }
}

$d = new Dog('Rex');
echo Animal::describe($d), "\n";
echo $d->hello(), "\n";
echo Status::fromLabel('on')->speak(), "\n";
echo Status::Active::class, "\n";
