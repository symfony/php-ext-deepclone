--TEST--
deepclone_dirty_candidates() leaves lazy ghosts untouched and rejects hooked properties
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) die('skip PHP 8.4 required'); ?>
--FILE--
<?php
class DirtyLazy
{
    public int $id;
}
class DirtyHooked
{
    public int $value = 1 {
        get => $this->value + 1;
    }
}
$reflection = new ReflectionClass(DirtyLazy::class);
$ghost = $reflection->newLazyGhost(static function (): void {
    throw new LogicException('initializer must not run');
});
var_dump(deepclone_dirty_candidates(DirtyLazy::class, ['id'], [$ghost], [spl_object_id($ghost) => ['id' => 1]]) === [$ghost]);
var_dump($reflection->isUninitializedLazyObject($ghost));
try {
    deepclone_dirty_candidates(DirtyHooked::class, ['value'], [], []);
} catch (ValueError $error) {
    echo "hooked plan rejected\n";
}
?>
--EXPECT--
bool(true)
bool(true)
hooked plan rejected
