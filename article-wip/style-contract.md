# Style contract — the optimization-ladder article

Voice: impersonal technical exposition. No "we", no "you", no "I". The subject of a
sentence is the decoder, the wire, the compiler, or a measurement.

## Structural rules

1. Every sentence carries a fact a reader could put in notes. Sentences whose job is
   transition, emphasis, recap, or morale are deleted, not improved.
2. Numbers are the only emphasis. A claim without a number or a mechanism is cut. The chart
   is the argument; prose states the mechanism and points at the chart.
3. A section opens with what the optimization IS (mechanism, 1-3 sentences), then what it
   changed (numbers, chart reference), then — only if true — where it does nothing and why.
   No closing summary sentence.
4. Sentence length varies. Two long sentences in a row is a flag; three is a rewrite.
5. At most one em-dash per sentence, used for an aside, never for cadence. Parentheses are
   fine for short asides.
6. Hedge only with measurements ("within the reference drift", "not resolved by this
   campaign"), never with vibes ("may", "can potentially", "somewhat").

## Banned outright

Intensifiers/verdicts: significantly, dramatically, drastically, blazingly, elegant,
powerful, robust, clean, beautifully, genuinely, remarkably, notably, impressively,
substantially, massive(ly), huge(ly).
Signposts: Note that, Importantly, Crucially, Interestingly, In essence, Essentially,
It's worth noting, The key insight, Let's, As we can see, Clearly, Obviously, Of course.
Constructions: "not just X, but Y"; "It's not X — it's Y"; rhetorical questions;
"Here's the thing"; "The result?"; any sentence that restates the previous one; triads
unless the set has exactly three members; "This means that" as a connective.

## Calibration protocol

The banned list above is a draft. One section (SWAR) plus the intro go to the author for
markup; every marked phrase becomes a new banned entry or rule before the remaining
sections are written. Existing repo docs are NOT a register reference (they are largely
model-worded); the author's markup is the only style corpus.

## Mechanical lint (before review)

grep for the banned list; em-dash count per 100 words <= 1.5; sentence-length variance
check (no 3-in-a-row within ±3 words); every "×"/"%"/"GB/s" claim must have a chart or
table within its section.
