<?php
// EXPECT: Larry Garfield
// EXPECT: Ilija Tovilo
// EXPECT: ok

// PHP 8.4 property hooks：全形式 get/set + backing 访问
class User {
    private string $first = "Larry";
    private string $last = "Garfield";
    public string $fullName {
        get => $this->first . " " . $this->last;
        set {
            $parts = explode(" ", $value, 2);
            $this->first = $parts[0];
            $this->last = $parts[1];
        }
    }
}
$u = new User();
echo $u->fullName, "\n";
$u->fullName = "Ilija Tovilo";
echo $u->fullName, "\n";
echo "ok\n";
