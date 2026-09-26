--TEST--
Heaps and MultipleIterator round-trip like with unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

class MyHeap extends SplMinHeap
{
    public $tag;
}

function state(object $o): array
{
    $state = [$o::class, (array) $o];
    if ($o instanceof SplPriorityQueue) {
        $state[] = $o->getExtractFlags();
        $o = clone $o;
        $o->setExtractFlags(SplPriorityQueue::EXTR_BOTH);
    }
    if ($o instanceof SplHeap || $o instanceof SplPriorityQueue) {
        $state[] = iterator_to_array(clone $o, false);
    } elseif ($o instanceof MultipleIterator) {
        $state[] = [$o->getFlags(), $o->countIterators()];
    }

    return $state;
}

$min = new SplMinHeap();
$min->insert(3);
$min->insert(1);
$max = new SplMaxHeap();
$max->insert([2, new stdClass()]);
$pq = new SplPriorityQueue();
$pq->insert('a', 1);
$pq->insert('b', 2);
$pq->setExtractFlags(SplPriorityQueue::EXTR_BOTH);
$sub = new MyHeap();
$sub->insert(5);
$sub->tag = 'x';
$mi = new MultipleIterator(MultipleIterator::MIT_KEYS_ASSOC);
$mi->attachIterator(new ArrayIterator([1]), 'a');

// Before PHP 8.5, heaps have no serialization API and unserialize() creates
// them empty, with their default flags; since, they keep their contents.
// MultipleIterator loses its iterators.
foreach ([$min, $max, $pq, $sub, $mi] as $o) {
    $clone = deepclone_from_array(deepclone_to_array($o));
    echo $o::class, ': ', var_export(state($clone) == state(unserialize(serialize($o))), true), "\n";
}

$c = deepclone_from_array(deepclone_to_array([$min, $min]));
var_dump($c[0] === $c[1]);

foreach ([SplMinHeap::class, SplMaxHeap::class, SplPriorityQueue::class, MultipleIterator::class] as $class) {
    $o = deepclone_hydrate($class);
    echo $class, ': ', $o instanceof MultipleIterator ? $o->countIterators() : count($o), "\n";
}
?>
--EXPECT--
SplMinHeap: true
SplMaxHeap: true
SplPriorityQueue: true
MyHeap: true
MultipleIterator: true
bool(true)
SplMinHeap: 0
SplMaxHeap: 0
SplPriorityQueue: 0
MultipleIterator: 0
