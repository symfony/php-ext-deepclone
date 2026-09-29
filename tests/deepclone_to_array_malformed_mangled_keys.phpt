--TEST--
deepclone_to_array() skips malformed mangled keys returned by __serialize() with the notices of unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

class S
{
    public function __construct(private $k) {}
    public function __serialize(): array { return [$this->k => 1, 'ok' => 2]; }
}

foreach (["\0", "\0*", "\0*\0", "\0A", "\0A\0", "\0\0", "\0*\0x", "\0S\0p"] as $k) {
    echo json_encode($k), ' ', json_encode(deepclone_to_array(new S($k))['properties']), "\n";
}

// The name of an anonymous class contains a NUL
$o = new class {
    private $a = 1;
    public $b = 2;
    public function set() { $this->a = 3; }
    public function get() { return $this->a; }
    public function __wakeup(): void {}
};
$o->set();
foreach ($o as $v) {
}
$clone = deepclone_from_array(deepclone_to_array($o));
var_dump($clone->get(), $clone->b);
?>
--EXPECTF--
"\u0000"%A
Notice: Illegal member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000*"%A
Notice: Illegal member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000*\u0000"%A
Notice: Corrupt member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000A"%A
Notice: Illegal member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000A\u0000"%A
Notice: Corrupt member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000\u0000"%A
Notice: Illegal member variable name in %s on line %d
{"stdClass":{"ok":[2]}}
"\u0000*\u0000x" {"stdClass":{"x":[1],"ok":[2]}}
"\u0000S\u0000p" {"S":{"p":[1]},"stdClass":{"ok":[2]}}
int(3)
int(2)
