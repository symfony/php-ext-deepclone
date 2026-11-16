--TEST--
deepclone_to_array() finds the private properties of anonymous classes that __sleep() returns by their key
--EXTENSIONS--
deepclone
--FILE--
<?php

$o = new class {
    private $a = 'default';
    protected $b = 'default';

    public function set($v): void { $this->a = $this->b = $v; }
    public function get(): array { return [$this->a, $this->b]; }
    public function __sleep(): array { return array_keys((array) $this); }
    public function __wakeup(): void {}
};
$o->set('changed');

var_dump(deepclone_from_array(deepclone_to_array($o))->get());

?>
--EXPECT--
array(2) {
  [0]=>
  string(7) "changed"
  [1]=>
  string(7) "changed"
}
