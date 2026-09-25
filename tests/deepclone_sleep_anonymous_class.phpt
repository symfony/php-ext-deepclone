--TEST--
deepclone_to_array() resolves bare __sleep() names to private properties of anonymous classes
--EXTENSIONS--
deepclone
--FILE--
<?php

$o = new class {
    private $a = 'default';
    public function set($v): void { $this->a = $v; }
    public function get() { return $this->a; }
    public function __sleep(): array { return ['a']; }
    public function __wakeup(): void {}
};
$o->set('changed');

$data = deepclone_to_array($o);
var_dump(array_values($data['properties'])[0]);

$o = new class {
    private $a = 'default';
    public function set($v): void { $this->a = $v; }
    public function __sleep(): array { return ['a']; }
    public function __unserialize(array $data): void { var_dump(array_values($data)); }
};
$o->set('changed');

deepclone_from_array(deepclone_to_array($o));
?>
--EXPECT--
array(1) {
  ["a"]=>
  array(1) {
    [0]=>
    string(7) "changed"
  }
}
array(1) {
  [0]=>
  string(7) "changed"
}
