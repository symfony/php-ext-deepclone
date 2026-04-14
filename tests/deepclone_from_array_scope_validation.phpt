--TEST--
deepclone_from_array() rejects invalid property scopes
--EXTENSIONS--
deepclone
--FILE--
<?php

class ScopeParent
{
    public int $pub = 0;
}

class ScopeChild extends ScopeParent
{
    protected int $sealed = 0;
    public int $child = 0;
}

try {
    deepclone_from_array([
        'classes' => 'ScopeChild',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['NoSuchScope' => ['pub' => [0 => 1]]],
    ]);
    var_dump(false);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'scope "NoSuchScope"'));
}

try {
    deepclone_from_array([
        'classes' => 'ScopeChild',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['ScopeParent' => ['child' => [0 => 2]]],
    ]);
    var_dump(false);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'ScopeParent::child'));
}

try {
    deepclone_from_array([
        'classes' => 'ScopeChild',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => ['sealed' => [0 => 3]]],
    ]);
    var_dump(false);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'non-public'));
}

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
Done
