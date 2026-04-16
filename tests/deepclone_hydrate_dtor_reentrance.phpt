--TEST--
deepclone_hydrate() survives a __destruct that reads the same property slot
--EXTENSIONS--
deepclone
--FILE--
<?php

// When the old value in a property slot has a __destruct that reads the
// same property, it must observe the NEW value (slot is already replaced
// before the dtor runs), not a half-freed zval.
class Host
{
    public ?ReadsSlotOnDtor $slot = null;
}

class ReadsSlotOnDtor
{
    public static ?string $observed = null;
    public string $tag = '';
    public ?Host $back = null;

    public function __destruct()
    {
        if ($this->back !== null) {
            self::$observed = $this->back->slot?->tag ?? 'null';
        }
    }
}

$host = new Host();

$old = new ReadsSlotOnDtor();
$old->tag = 'old';
$old->back = $host;
$host->slot = $old;
unset($old); // slot holds the only ref; next overwrite triggers dtor.

$new = new ReadsSlotOnDtor();
$new->tag = 'new';
$new->back = $host;

// Hydrate $host with slot = $new. The assignment frees the old object,
// whose __destruct reads $host->slot. That read must observe $new, not
// a freed zval.
deepclone_hydrate($host, ['slot' => $new]);

var_dump($host->slot->tag === 'new');
var_dump(ReadsSlotOnDtor::$observed === 'new');

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
Done
