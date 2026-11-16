--TEST--
deepclone_to_array() walks the value a reference had when it met it, whatever user code assigns to the reference during the walk
--EXTENSIONS--
deepclone
--FILE--
<?php

class Sleeper
{
    public $x = 1;
    public function __sleep(): array { $GLOBALS['r'] = 123; return ['x']; }
}

class Serializer
{
    public $x = 2;
    public function __serialize(): array { $GLOBALS['r'] = 'gone'; return ['x' => $this->x]; }
}

class State
{
    public $x = 3;
    public function __serialize(): array { $GLOBALS['r'] = null; return ['x' => $this->x]; }
    public function __unserialize(array $data): void { $this->x = $data['x']; }
}

foreach ([new Sleeper(), new Serializer(), new State()] as $r) {
    $arr = [&$r, &$r];
    $clone = deepclone_from_array(deepclone_to_array($arr));
    var_dump($r, $clone[0]->x, $clone[0] === $clone[1]);
    unset($r, $arr);
}

if (PHP_VERSION_ID >= 80400) {
    class Lazy { public $x = 0; }
    $r = (new ReflectionClass(Lazy::class))->newLazyGhost(function ($o) { $GLOBALS['r'] = []; $o->x = 4; });
    $arr = [&$r, &$r];
    $clone = deepclone_from_array(deepclone_to_array($arr));
    var_dump($r, $clone[0]->x, $clone[0] === $clone[1]);
} else {
    echo "array(0) {\n}\nint(4)\nbool(true)\n";
}

?>
--EXPECT--
int(123)
int(1)
bool(true)
string(4) "gone"
int(2)
bool(true)
NULL
int(3)
bool(true)
array(0) {
}
int(4)
bool(true)
