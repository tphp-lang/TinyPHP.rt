<?php
// enum：unit/backed、::from/tryFrom、cases、方法、int backed
// EXPECT: active: H
// EXPECT: closed-from: S
// EXPECT: tryFrom-bad: none
// EXPECT: cases: 2, H,S
// EXPECT: label: suit-H
// EXPECT: unit: One,Two
// EXPECT: unit-name: Two
// EXPECT: score: 5

enum Suit: string {
    case Hearts = "H";
    case Spades = "S";

    public function label() { return "suit-{$this->value}"; }
}

$s = Suit::Hearts;
echo "active: ", $s->value, "\n";

$closed = Suit::from("S");
echo "closed-from: ", $closed->value, "\n";

$bad = Suit::tryFrom("X") ?? null;
echo "tryFrom-bad: ", $bad === null ? "none" : $bad->value, "\n";

$all = Suit::cases();
echo "cases: ", count($all), ", ", $all[0]->value, ",", $all[1]->value, "\n";

echo "label: ", $s->label(), "\n";

enum Level {
    case One;
    case Two;
}

$o = Level::One;
$t = Level::Two;
echo "unit: ", $o->name, ",", $t->name, "\n";
echo "unit-name: ", Level::Two->name, "\n";

enum Score: int {
    case Low = 3;
    case High = 5;
}
echo "score: ", Score::High->value, "\n";
