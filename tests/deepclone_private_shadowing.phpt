--TEST--
deepclone_hydrate / from_array handle same-named private in parent and child as distinct slots
--EXTENSIONS--
deepclone
--FILE--
<?php

class PrivShadowA
{
    private string $x = 'a_init';
    public function get(): string { return $this->x; }
}

class PrivShadowB extends PrivShadowA
{
    private string $x = 'b_init';
    public function getChild(): string { return $this->x; }
}

// 1) Per-scope hydrate writes to distinct slots.
$b = new PrivShadowB();
deepclone_hydrate($b, [
    PrivShadowA::class => ['x' => 'A_written'],
    PrivShadowB::class => ['x' => 'B_written'],
]);
var_dump($b->get() === 'A_written');
var_dump($b->getChild() === 'B_written');

// 2) Roundtrip through to_array/from_array preserves both slots independently.
$orig = new PrivShadowB();
$ref = new \ReflectionProperty(PrivShadowA::class, 'x');
$ref->setValue($orig, 'parent_val');
$ref2 = new \ReflectionProperty(PrivShadowB::class, 'x');
$ref2->setValue($orig, 'child_val');

$clone = deepclone_from_array(deepclone_to_array($orig));
var_dump($clone->get() === 'parent_val');
var_dump($clone->getChild() === 'child_val');

// 3) Bare name in $mangled_vars targets the most-derived private slot.
$b2 = new PrivShadowB();
deepclone_hydrate($b2, [], ['x' => 'bare_val']);
var_dump($b2->get() === 'a_init');         // parent untouched
var_dump($b2->getChild() === 'bare_val');  // child written

// 4) Explicit mangled key targets the parent's private slot.
$b3 = new PrivShadowB();
deepclone_hydrate($b3, [], ["\0PrivShadowA\0x" => 'parent_targeted']);
var_dump($b3->get() === 'parent_targeted');
var_dump($b3->getChild() === 'b_init');

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
