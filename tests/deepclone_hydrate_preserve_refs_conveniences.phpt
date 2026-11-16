--TEST--
deepclone_hydrate() converts the values of the references it preserves like other values
--EXTENSIONS--
deepclone
--FILE--
<?php

enum Suit: string
{
    case Hearts = 'H';
    case Spades = 'S';
}

class Card
{
    public int $rank = 0;
    public Suit $suit = Suit::Hearts;
    public int $count = 1;
}

// With PRESERVE_REFS, the values of references are converted like other values
$rank = '5';
$suit = 'S';
$count = null;
$card = deepclone_hydrate(new Card(), ['rank' => &$rank, 'suit' => &$suit, 'count' => &$count], DEEPCLONE_HYDRATE_PRESERVE_REFS);
var_dump($rank, $suit, $count);
var_dump($card);

$rank = 6;
$suit = Suit::Hearts;
var_dump($card->rank, $card->suit);

try {
    $bad = 'abc';
    deepclone_hydrate(new Card(), ['rank' => &$bad], DEEPCLONE_HYDRATE_PRESERVE_REFS);
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

?>
--EXPECTF--
int(5)
enum(Suit::Spades)
NULL
object(Card)#%d (2) {
  ["rank"]=>
  &int(5)
  ["suit"]=>
  &enum(Suit::Spades)
  ["count"]=>
  uninitialized(int)
}
int(6)
enum(Suit::Hearts)
Cannot assign string to property Card::$rank of type int
