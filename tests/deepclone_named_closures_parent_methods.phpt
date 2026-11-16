--TEST--
Closures over a method of a parent class that the class of the object overrides are restored as such
--EXTENSIONS--
deepclone
--FILE--
<?php

class A
{
    public function pub() { return 'A::pub'; }
    protected function prot() { return 'A::prot'; }
    public static function spub() { return 'A::spub called on '.static::class; }
    protected static function sprot() { return 'A::sprot called on '.static::class; }
}

class B extends A
{
    public function pub() { return 'B::pub'; }
    protected function prot() { return 'B::prot'; }
    public static function spub() { return 'B::spub called on '.static::class; }
    protected static function sprot() { return 'B::sprot called on '.static::class; }

    public function closures(): array
    {
        return [parent::pub(...), parent::prot(...), parent::spub(...), parent::sprot(...), $this->pub(...), self::spub(...), A::spub(...)];
    }
}

class C extends B
{
}

// A closure over the method of a parent class that the class of the object overrides is restored as such
foreach ((new C())->closures() as $closure) {
    $payload = deepclone_to_array($closure, null, true);
    $clone = deepclone_from_array($payload, null, true);
    echo $closure(), ' => ', $clone(), '   ', json_encode($payload['prepared']), "\n";
}

?>
--EXPECT--
A::pub => A::pub   [[0,"pub"],"A","pub"]
A::prot => A::prot   [[0,"prot"],"A","prot"]
A::spub called on C => A::spub called on C   [["C","spub"],"A","spub"]
A::sprot called on C => A::sprot called on C   [["C","sprot"],"A","sprot"]
B::pub => B::pub   [0,"pub"]
B::spub called on C => B::spub called on C   ["C","spub"]
A::spub called on A => A::spub called on A   ["A","spub"]
