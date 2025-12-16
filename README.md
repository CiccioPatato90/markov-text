## What is this?
This is an attempt to generate text using Markov chains in C.

It consists of a database of text, in .txt format, which is used to "train" the generator.

The file is opened, parsed and most importantly Tokenized.
For now the tokenization is fairly simple: it discards everything that is not a letter and extracts a CaptureGroup (from regex world) based on spaces. It assumes that words are separated by spaces.
An interesting feature to add could be to add punctuation marks to the semantics of the tokenizer by recursively separating groups of tokens and introducing the Period, SubPeriod and other logical language constructs.

After the tokenization we use a simple C Map (could also use a WordTrie but did not come around to implement it).
This Map has as keys the N-Degree Markov Token and then as values a dynamic array of transition tokens.
We choose randomly among them for the next token to insert to the chain.

It would be interesting to add conversational abilities to the generator.
The question could be parsed in the same way as the text, and then I DON'T KNOW.
Thanks for the attention!
