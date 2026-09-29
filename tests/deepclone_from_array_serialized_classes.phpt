--TEST--
deepclone_from_array() handles serialized objects that reference themselves or throw
--EXTENSIONS--
deepclone
--FILE--
<?php

@eval('class OldSer implements Serializable {
    public $x;
    public function serialize(): string { return serialize($this->x); }
    public function unserialize($data): void { $this->x = unserialize($data); }
}');

class SerRef
{
    public $a;
    public function __serialize(): array { return ['a' => &$this->a]; }
    public function __unserialize(array $data): void { $this->a = &$data['a']; }
}

// The payload holds C:6:"OldSer":...{...R:1;}, which unserializes to a reference
$o = new OldSer();
$o->x = new SerRef();
$o->x->a = $o;
$c = deepclone_from_array(deepclone_to_array($o));
var_dump($c->x->a === $c);

class UnThrow
{
    public function __unserialize(array $data): void { throw new DomainException('boom'); }
}

try {
    deepclone_from_array(['classes' => ['stdClass', 'O:7:"UnThrow":0:{}'], 'objectMeta' => [0, 1], 'prepared' => [0, 1], 'mask' => [true, true]]);
} catch (DomainException $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
bool(true)
boom
