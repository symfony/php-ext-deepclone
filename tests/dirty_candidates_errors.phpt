--TEST--
deepclone_dirty_candidates() rejects malformed or unsupported property plans
--EXTENSIONS--
deepclone
--FILE--
<?php
class DirtyParent
{
    private int $hidden = 1;
}
class DirtyChild extends DirtyParent
{
    public int $id = 1;
    public static int $ignored = 1;
}

$record = new DirtyChild();
foreach ([
    fn () => deepclone_dirty_candidates('UnknownDirtyClass', [], [], []),
    fn () => deepclone_dirty_candidates(stdClass::class, [], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, [1 => 'id'], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, ['id', 'id'], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, ['ignored'], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, ['hidden'], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, ['missing'], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, [1], [], []),
    fn () => deepclone_dirty_candidates(DirtyChild::class, [], [$record, false], []),
] as $scan) {
    try {
        $scan();
        echo "missing exception\n";
    } catch (Throwable $error) {
        echo $error::class, "\n";
    }
}
?>
--EXPECT--
DeepClone\ClassNotFoundException
ValueError
ValueError
ValueError
ValueError
ValueError
ValueError
ValueError
TypeError
