--TEST--
deepclone_dirty_candidates() conservatively compares declared properties with snapshots
--EXTENSIONS--
deepclone
--FILE--
<?php
class DirtyRecord
{
    private int $id = 1;
    public ?string $note = null;
    public array $payload = [];
    public DateTimeImmutable $date;
    public int $status;
}

class OtherDirtyRecord extends DirtyRecord {}

function createRecord(): DirtyRecord
{
    $record = new DirtyRecord();
    $record->date = new DateTimeImmutable('2026-11-26');
    $record->payload = ['nested' => [1, 2]];
    $record->status = 7;

    return $record;
}

$same = createRecord();
$changed = createRecord();
$missing = createRecord();
$uninitialized = createRecord();
$referenced = createRecord();
$other = new OtherDirtyRecord();
$separateArray = createRecord();
$equalDate = createRecord();
$recursive = createRecord();
$cyclic = [];
$cyclic['self'] =& $cyclic;
$recursive->payload = $cyclic;
$records = compact('same', 'changed', 'missing', 'uninitialized', 'referenced', 'other', 'separateArray', 'equalDate', 'recursive');
$snapshots = [];
foreach ($records as $name => $record) {
    if ($name !== 'other' && $name !== 'missing') {
        $snapshots[spl_object_id($record)] = [
            'id' => 1,
            'note' => $record->note,
            'payload' => $record->payload,
            'date' => $record->date,
            'status' => $record->status,
        ];
    }
}
$changed->note = 'changed';
unset($uninitialized->status);
$ref = 7;
$referenced->status =& $ref;
$separateArray->payload = unserialize(serialize($separateArray->payload));
$equalDate->date = clone $equalDate->date;
$distinctCycle = [];
$distinctCycle['self'] =& $distinctCycle;
$recursive->payload = $distinctCycle;

$candidates = deepclone_dirty_candidates(DirtyRecord::class, ['id', 'note', 'payload', 'date', 'status'], $records, $snapshots);
foreach ($candidates as $record) {
    echo array_search($record, $records, true), "\n";
}
var_dump(deepclone_dirty_candidates(DirtyRecord::class, ['note'], [$same], []) === [$same]);
?>
--EXPECT--
changed
missing
uninitialized
referenced
other
separateArray
equalDate
recursive
bool(true)
