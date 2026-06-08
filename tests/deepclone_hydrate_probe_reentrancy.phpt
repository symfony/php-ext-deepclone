--TEST--
deepclone_hydrate() instantiability probe stays isolated when called inside another unserialize() (symfony/symfony#64323)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (!class_exists('BcMath\Number')) die('skip requires BcMath\Number (PHP 8.4+ with ext/bcmath)'); ?>
--INI--
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php
use BcMath\Number;

// deepclone_hydrate() probes instantiability by running an inner unserialize().
// When invoked from inside an active inline unserialize() (Serializable::
// unserialize() here, which does not raise serialize_lock), that probe must
// stay isolated: it must still reject an uninstantiable class, and must not
// leak its throwaway object into the outer unserialize() and corrupt it.
class Holder implements Serializable {
    public string $result = '';
    public function serialize(): ?string { return ''; }
    public function unserialize(string $data): void {
        try {
            deepclone_hydrate(Number::class);
            $this->result = 'allowed';
        } catch (\DeepClone\NotInstantiableException $e) {
            $this->result = 'refused';
        }
    }
}

$h = unserialize('C:6:"Holder":0:{}');
var_dump($h instanceof Holder); // the outer unserialize() succeeded
var_dump($h->result);           // the probe still rejected the class

// The outer context is not corrupted: a real BcMath\Number unserialize() that
// runs afterwards (its deferred __unserialize fires in the same var_destroy)
// works cleanly.
$n = unserialize(serialize(new Number('3.5')));
var_dump((string) $n, $n->scale);
?>
--EXPECT--
bool(true)
string(7) "refused"
string(3) "3.5"
int(1)
