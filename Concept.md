# Mini Lisp Workflow Language

## Core Philosophy

-   Everything is an expression
    
-   Prefix notation
    
-   Small runtime
    
-   JSON-friendly
    
-   Easy to embed in JS/Rust/C
    
-   Good for workflows and automation
    

---

# Basic Syntax

## Function Call

``` lisp
(op arg1 arg2 arg3)
```

Example:

``` lisp
(+ 1 2)  
(* 10 20)  
(print "hello")
```

---

# Blocks

A block evaluates expressions sequentially.

``` lisp
(do  
    (print "hello")  
    (print "world")  
)
```

`do` returns the last expression.

---

# Variables

## Set Variable

``` lisp
(set name "vivek")  
(set age 30)
```

## Get Variable

``` lisp
(get name)
```

Short form:

``` lisp
name
```

---

# Data Types

## Numbers

``` lisp
10  
20.5  
-100
```

## String

``` lisp
"hello"
```

UTF-8 supported.

---

## Boolean

``` lisp
true  
false
```

---

## Null

``` lisp
null
```

---

## Array

``` lisp
[array 1 2 3]
```

or

``` lisp
(array 1 2 3)
```

---

## Object

``` lisp
(object  
    "name" "vivek"  
    "age" 30  
)
```

Result:

```
JSON
{  
  "name": "vivek",  
  "age": 30  
}
```

```

---

# Arithmetic Operators

``` lisp
(+ 1 2)  
(- 10 5)  
(* 2 3)  
(/ 10 2)  
(% 10 3)
```

---

# Comparison Operators

``` lisp
(> 10 5)  
(< 10 5)  
(>= 10 10)  
(<= 5 10)  
(== 10 10)  
(!= 10 5)
```

---

# Logical Operators

``` lisp
(&& true false)  
(|| true false)  
(! true)
```

---

# Conditionals

## If

``` lisp
(if (> age 18)  
    (print "adult")  
    (print "child")  
)
```

---

# Loops

## While

``` lisp
(while (< x 10)  
    (do  
        (print x)  
        (set x (+ x 1))  
    )  
)
```

---

## For Each

``` lisp
(for item items  
    (print item)  
)
```

---

# Functions

## Define Function

``` lisp
(fn add (a b)  
    (+ a b)  
)
```

---

## Call Function

``` lisp
(add 10 20)
```

---

# Anonymous Functions

``` lisp
(lambda (x)  
    (* x 2)  
)
```

---

# Arrays

## Create

``` lisp
(set nums (array 1 2 3))
```

## Access

``` lisp
(get nums 0)
```

## Push

``` lisp
(push nums 4)
```

## Length

``` lisp
(length nums)
```

---

# Object Operations

## Get Field

``` lisp
(get user "name")
```

---

## Set Field

``` lisp
(set user.name "vivek")
```

or

``` lisp
(put user "name" "vivek")
```

---

## Keys

``` lisp
(keys user)
```

---

## Values

``` lisp
(values user)
```

---

## Entries

``` lisp
(entries user)
```

---

# String Operations

``` lisp
(concat "hello" "world")  
(split "," "a,b,c")  
(trim " hello ")  
(lower "ABC")  
(upper "abc")
```

---

# File Operations

## Read File

``` lisp
(read "data.txt")
```

---

## Write File

``` lisp
(write "out.txt" "hello")
```

---

## Append File

``` lisp
(append "log.txt" "new line")
```

---

## Delete File

``` lisp
(delete "tmp.txt")
```

---

# HTTP Operations

## GET

``` lisp
(http:get "https://example.com")
```

---

## POST

``` lisp
(http:post  
    "https://api.test.com"  
    (object  
        "name" "vivek"  
    )  
)
```

---

# JSON

## Parse

``` lisp
(json:parse text)
```

---

## Stringify

``` lisp
(json:stringify obj)
```

---

# Error Handling

## Try Catch

``` lisp
(try  
    (do  
        (dangerous-op)  
    )  
    (catch err  
        (print err)  
    )  
)
```

---

# Modules

## Import

``` lisp
(import "math.dsl")
```

---

# Comments

## Single Line

``` lisp
; this is comment
```

---

# Special Forms

These do NOT evaluate all arguments automatically.

| Form | Description |
| --- | --- |
| if | conditional |
| while | loop |
| fn | define function |
| lambda | anonymous function |
| set | assignment |
| try | exception handling |
| do | sequential execution |

---

# Execution Model

## AST Example

Code:

``` lisp
(+ 1 2)
```

AST:

```
JSON
{  
  "type": "call",  
  "op": "+",  
  "args": [1, 2]  
}

```

```ebnf

---

# Grammar (EBNF)



 
program     = expr* ;  
  
expr        =  
      number  
    | string  
    | boolean  
    | null  
    | symbol  
    | list ;  
  
list        = "(" expr* ")" ;  
  
symbol      = letter { letter | digit | "_" | ":" | "." } ;  
  
number      = digit+ [ "." digit+ ] ;  
  
string      = '"' { char } '"' ;
```

---

# Example Program

``` lisp
(do  
    (set users  
        (array  
            (object "name" "Alice" "age" 20)  
            (object "name" "Bob" "age" 15)  
        )  
    )  
  
    (for user users  
        (if (> (get user "age") 18)  
            (print  
                (concat  
                    (get user "name")  
                    " is adult"  
                )  
            )  
        )  
    )  
)
```

---

# Recommended Built-in Standard Library

## Core

-   print
    
-   type
    
-   assert
    
-   sleep
    

## Math

-   min
    
-   max
    
-   floor
    
-   ceil
    
-   random
    

## Array

-   map
    
-   filter
    
-   reduce
    
-   push
    
-   pop
    

## String

-   replace
    
-   substring
    
-   regex
    

## System

-   env
    
-   exec
    
-   exit
    

## Time

-   now
    
-   timestamp
    
-   format-date
    

---

# Good Future Features

## Async

``` lisp
(await (http:get url))
```

---

## Parallel Execution

``` lisp
(parallel  
    (task1)  
    (task2)  
)
```

---

## Pattern Matching

``` lisp
(match value  
    1 "one"  
    2 "two"  
    _ "unknown"  
)
```

---

# Recommended Internal Architecture

Parser → AST → Evaluator

```
Source Code  
    ↓  
Tokenizer  
    ↓  
Parser  
    ↓  
AST  
    ↓  
Interpreter / VM
```

---

# Suggested Reserved Keywords

```
if  
while  
for  
fn  
lambda  
do  
set  
true  
false  
null  
try  
catch  
import  
return  
break  
continue
```

---

# Suggested File Extension

```
.dsl  
.lsp  
.flow  
.dag
```

---

# Example Style Guide

## Good

``` lisp
(print (+ 10 20))
```

## Bad

``` lisp
(print(+10 20))
```

---

 

 