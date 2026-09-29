--TEST--
deepclone_hydrate() binds references to untyped properties with CALL_HOOKS, and leaves the properties of internal classes initialized
--EXTENSIONS--
deepclone
--FILE--
<?php

class T
{
    public $u;
    public int $i = 0;
}

foreach ([0, DEEPCLONE_HYDRATE_PRESERVE_REFS, DEEPCLONE_HYDRATE_CALL_HOOKS | DEEPCLONE_HYDRATE_PRESERVE_REFS] as $flags) {
    $v = 1.5;
    $vars = ['u' => &$v, 'i' => &$w];
    $w = 3;
    $o = deepclone_hydrate('T', $vars, $flags);
    $v = 2;
    $w = 4;
    echo $flags, ': ', json_encode([$o->u, $o->i]), "\n";
}

// null leaves the non-nullable properties of user classes uninitialized only
try {
    deepclone_hydrate('Exception', ["\0Exception\0trace" => null]);
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}
var_dump(isset(deepclone_hydrate('T', ['i' => null])->i));
?>
--EXPECT--
0: [1.5,3]
4: [2,4]
5: [2,4]
Cannot assign null to property Exception::$trace of type array
bool(false)
