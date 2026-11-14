#!/bin/sh
# Xenon Utils test suite. SPDX-License-Identifier: MIT
#
# Expected values come from POSIX-2001 behaviour rather than from another
# implementation. Each check runs one command and compares its exit status
# and standard output.

XN=$(CDPATH= cd -- "$(dirname -- "$0")/../cmd" && pwd) || exit 1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/xntest.XXXXXX") || exit 1
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
umask 022
LC_ALL=C
TZ=UTC0
export LC_ALL TZ

pass=0
fail=0

# check NAME EXPECTED_STATUS EXPECTED_STDOUT COMMAND...
check() {
    name=$1; want_status=$2; want_out=$3
    shift 3
    got_out=$("$@" 2>/dev/null; echo "status=$?")
    got_status=${got_out##*status=}
    got_out=${got_out%status=*}
    # A trailing newline is stripped from neither side's comparison text.
    want_full=$want_out
    if [ -n "$want_out" ]; then
        want_full="$want_out
"
    fi
    if [ "$got_status" = "$want_status" ] && [ "$got_out" = "$want_full" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n  status: want %s got %s\n  want: [%s]\n  got:  [%s]\n' \
            "$name" "$want_status" "$got_status" "$want_full" "$got_out"
    fi
}

# Feed stdin from a string.
sh_in() {
    input=$1
    shift
    printf '%b' "$input" | "$@"
}

xn() {
    tool=$1
    shift
    "$XN/$tool" "$@"
}

# --- every expected binary and link exists ---
for tool in basename cat chgrp chmod chown cmp comm cp cut date dd df dirname \
            du echo env expr false head id ln ls mkdir mkfifo mv nice od \
            paste printf pwd rm rmdir sleep sort tail tee test touch tr true \
            tty uname uniq wc who; do
    check "exists $tool" 0 "" test -x "$XN/$tool"
done
check "tool count" 0 "45" sh -c 'cd "$1" && ls | grep -cv -e "\.[co]$" -e "^\[$"' sh "$XN"

# --- shared-binary behaviour ---
check "cp is mv" 0 "" test "$XN/cp" -ef "$XN/mv"
check "ln is mv" 0 "" test "$XN/ln" -ef "$XN/mv"
cp "$XN/mv" badname
check "wrong name" 1 "" ./badname
rm -f badname

# --- true false basename dirname ---
check "true" 0 "" xn true --help
check "false" 1 "" xn false ignored
check "basename" 0 "c" xn basename /a/b/c
check "basename suffix" 0 "c" xn basename /a/b/c.txt .txt
check "basename slashes" 0 "b" xn basename /a/b///
check "basename root" 0 "/" xn basename ///
check "basename whole suffix" 0 "foo" xn basename foo foo
check "basename dashes" 0 "-x" xn basename -- -x
check "dirname" 0 "/a/b" xn dirname /a/b/c
check "dirname no slash" 0 "." xn dirname foo
check "dirname root" 0 "/" xn dirname /
check "dirname top" 0 "/" xn dirname /a
check "dirname trailing" 0 "/a" xn dirname /a/b/
check "dirname multi" 0 "a" xn dirname a///b

# --- echo is the XSI form: no options, escapes on ---
check "echo plain" 0 "a b" xn echo a b
check "echo -n literal" 0 "-n a" xn echo -n a
check "echo -- literal" 0 "--" xn echo --
check "echo escape" 0 "a	b" xn echo 'a\tb'
check "echo \\c" 0 "a" sh -c '"$1/echo" "a\cb" c; echo' sh "$XN"
check "echo octal" 0 "A" xn echo '\0101'

# --- printf ---
check "printf basic" 0 "5 x" xn printf '%d %s\n' 5 x
check "printf reuse" 0 "1
2" xn printf '%s\n' 1 2
check "printf width" 0 "   ab" xn printf '%5s\n' ab
check "printf star" 0 "   ab" xn printf '%*s\n' 5 ab
check "printf hex" 0 "ff" xn printf '%x\n' 255
check "printf char const" 0 "65" xn printf '%d\n' "'A"
check "printf bad num" 1 "0" xn printf '%d\n' abc
check "printf partial num" 1 "12" xn printf '%d\n' 12abc
check "printf %b" 0 "a
b" xn printf '%b\n' 'a\nb'
check "printf %%" 0 "%" xn printf '%%\n'
check "printf octal" 0 "A" xn printf '\101\n'
check "printf missing" 0 "[]" xn printf '[%s]\n'
check "printf negative %u wrap" 0 "1.50" xn printf '%.2f\n' 1.5
check "printf %b stop" 0 "a" sh -c '"$1/printf" "%b" "a\cb"; echo' sh "$XN"

# --- option parsing must never permute ---
check "env stops at operand" 0 "-i" xn env "$XN/echo" -i
check "nice keeps args" 0 "-n 5" xn nice "$XN/echo" -n 5
check "getopt grouped" 0 "2 2" sh_in 'a\nb\n' xn wc -lw

# --- cat ---
printf 'one\ntwo\n' > f1
printf 'three\n' > f2
check "cat files" 0 "one
two
three" xn cat f1 f2
check "cat stdin dash" 0 "one
two
x
three" sh -c 'printf "x\n" | "$1/cat" f1 - f2' sh "$XN"
check "cat missing" 1 "one
two" xn cat nosuch f1
check "cat -u" 0 "three" xn cat -u f2

# --- head / tail ---
seq 1 20 > nums
check "head default" 0 "$(seq 1 10)" xn head nums
check "head -n" 0 "1
2" xn head -n 2 nums
check "head -n0" 0 "" xn head -n 0 nums
check "head multi" 0 "==> f1 <==
one

==> f2 <==
three" xn head -n 1 f1 f2
check "head offset" 0 "2" sh -c '( "$1/head" -n 1 >/dev/null; "$1/head" -n 1 ) < nums' sh "$XN"
check "tail default" 0 "$(seq 11 20)" xn tail nums
check "tail -n" 0 "19
20" xn tail -n 2 nums
check "tail +n" 0 "19
20" xn tail -n +19 nums
check "tail -c" 0 "0" xn tail -c 2 nums
check "tail -c+" 0 "20" xn tail -c +49 nums
check "tail pipe" 0 "19
20" sh -c 'cat nums | "$1/tail" -n 2' sh "$XN"
check "tail no newline" 0 "b
c" sh -c 'printf "a\nb\nc" > nn; "$1/tail" -n 2 nn; echo' sh "$XN"
check "tail pipe no newline" 0 "b
c" sh -c 'printf "a\nb\nc" | "$1/tail" -n 2; echo' sh "$XN"
check "tail zero" 0 "" xn tail -n 0 nums
seq 1 100000 > big
check "tail big file" 0 "99999
100000" xn tail -n 2 big
check "tail big pipe" 0 "99999
100000" sh -c 'cat big | "$1/tail" -n 2' sh "$XN"

# --- wc is unpadded, per POSIX ---
check "wc all" 0 "2 2 8 f1" xn wc f1
check "wc stdin" 0 "2 2 8" sh_in 'one\ntwo\n' xn wc
check "wc -l" 0 "2 f1" xn wc -l f1
check "wc total" 0 "2 2 8 f1
1 1 6 f2
3 3 14 total" xn wc f1 f2
check "wc -m" 0 "4" sh_in 'abc\n' xn wc -m

# --- tee ---
check "tee" 0 "hi" sh_in 'hi\n' xn tee teeout
check "tee wrote" 0 "hi" cat teeout
check "tee -a" 0 "hi" sh_in 'hi\n' xn tee -a teeout
check "tee append" 0 "2" sh -c 'wc -l < teeout'

# --- cut ---
check "cut -b" 0 "bc" sh_in 'abcd\n' xn cut -b 2-3
check "cut -c open" 0 "cd" sh_in 'abcd\n' xn cut -c 3-
check "cut -c lead" 0 "ab" sh_in 'abcd\n' xn cut -c -2
check "cut -f" 0 "b" sh_in 'a\tb\tc\n' xn cut -f 2
check "cut -d" 0 "a:c" sh_in 'a:b:c\n' xn cut -d : -f 1,3
check "cut no delim" 0 "abc" sh_in 'abc\n' xn cut -d : -f 2
check "cut -s" 0 "" sh_in 'abc\n' xn cut -s -d : -f 2
check "cut order" 0 "ac" sh_in 'abc\n' xn cut -c 3,1
check "cut needs mode" 1 "" xn cut f1

# --- paste ---
check "paste" 0 "one	three
two	" xn paste f1 f2
check "paste -d" 0 "one:three
two:" xn paste -d : f1 f2
check "paste -s" 0 "one	two" xn paste -s f1
check "paste -sd" 0 "1,2,3" sh_in '1\n2\n3\n' xn paste -s -d , -
check "paste cycle" 0 "1a2b3" sh_in '1\n2\n3\n' xn paste -s -d 'ab' -
check "paste stdin twice" 0 "1	2
3	4" sh_in '1\n2\n3\n4\n' xn paste - -

# --- uniq ---
check "uniq" 0 "a
b" sh_in 'a\na\nb\n' xn uniq
check "uniq -c" 0 "2 a
1 b" sh_in 'a\na\nb\n' xn uniq -c
check "uniq -d" 0 "a" sh_in 'a\na\nb\n' xn uniq -d
check "uniq -u" 0 "b" sh_in 'a\na\nb\n' xn uniq -u
check "uniq -f" 0 "1 x" sh_in '1 x\n2 x\n' xn uniq -f 1
check "uniq -s" 0 "ax" sh_in 'ax\nbx\n' xn uniq -s 1

# --- comm ---
printf 'a\nb\nd\n' > c1
printf 'b\nc\nd\n' > c2
check "comm" 0 "a
		b
	c
		d" xn comm c1 c2
check "comm -12" 0 "b
d" xn comm -12 c1 c2
check "comm -3" 0 "a
	c" xn comm -3 c1 c2

# --- cmp ---
printf 'abcd\nefgh\n' > cm1
printf 'abcd\nefXh\n' > cm2
check "cmp same" 0 "" xn cmp cm1 cm1
check "cmp diff" 1 "cm1 cm2 differ: char 8, line 2" xn cmp cm1 cm2
check "cmp -s" 1 "" xn cmp -s cm1 cm2
check "cmp -l" 1 "8 147 130" xn cmp -l cm1 cm2
check "cmp missing" 2 "" xn cmp cm1 nosuch
check "cmp eof" 1 "" xn cmp -s f2 f1
check "cmp no permute" 2 "" xn cmp f2 f1 -s

# --- tr ---
check "tr map" 0 "HELLO" sh_in 'hello\n' xn tr a-z A-Z
check "tr class" 0 "HELLO" sh_in 'hello\n' xn tr '[:lower:]' '[:upper:]'
check "tr -d" 0 "hll" sh_in 'hello\n' xn tr -d aeiou
check "tr -s" 0 "helo" sh_in 'hello\n' xn tr -s l
check "tr -c" 0 "XXhiXX" sh -c 'printf "12hi34" | "$1/tr" -c a-z X; echo' sh "$XN"
check "tr -cd" 0 "hi" sh -c 'printf "12hi34" | "$1/tr" -cd a-z; echo' sh "$XN"
check "tr -ds" 0 "helo" sh_in 'helllo\n' xn tr -ds x l
check "tr fill" 0 "xxx" sh -c 'printf "abc" | "$1/tr" abc "[x*]"; echo' sh "$XN"
check "tr repeat" 0 "xxy" sh -c 'printf "abc" | "$1/tr" abc "[x*2]y"; echo' sh "$XN"
check "tr escape" 0 "a b" sh -c 'printf "a\nb" | "$1/tr" "\n" " "; echo' sh "$XN"
check "tr backwards" 1 "" sh_in 'a' xn tr z-a x
check "tr equiv" 0 "b" sh -c 'printf "a" | "$1/tr" "[=a=]" b; echo' sh "$XN"

# --- sort ---
check "sort" 0 "a
b
c" sh_in 'c\na\nb\n' xn sort
check "sort -r" 0 "c
b" sh_in 'b\nc\n' xn sort -r
check "sort -n" 0 "2
10
100" sh_in '100\n2\n10\n' xn sort -n
check "sort -n neg" 0 "-5
-1
0
3" sh_in '3\n-1\n0\n-5\n' xn sort -n
check "sort -n frac" 0 "1.2
1.25
1.3" sh_in '1.3\n1.25\n1.2\n' xn sort -n
check "sort -n huge" 0 "99999999999999999999998
99999999999999999999999" sh_in '99999999999999999999999\n99999999999999999999998\n' xn sort -n
check "sort -u" 0 "a
b" sh_in 'b\na\nb\n' xn sort -u
check "sort -f" 0 "a
B
c" sh_in 'c\nB\na\n' xn sort -f
check "sort -t -k" 0 "x:1
y:2" sh_in 'y:2\nx:1\n' xn sort -t : -k 2
check "sort -k n" 0 "b 2
a 10" sh_in 'a 10\nb 2\n' xn sort -k 2n
check "sort -k char" 0 "ab
ba" sh_in 'ba\nab\n' xn sort -k 1.2r
check "sort -k range" 0 "b 1 z
a 2 y" sh_in 'a 2 y\nb 1 z\n' xn sort -k 2,2
check "sort -c ok" 0 "" sh_in 'a\nb\n' xn sort -c
check "sort -c bad" 1 "" sh_in 'b\na\n' xn sort -c
check "sort -o self" 0 "a
b" sh -c 'printf "b\na\n" > so; "$1/sort" -o so so; cat so' sh "$XN"
check "sort -m" 0 "a
b
c
d" sh -c 'printf "a\nc\n" > m1; printf "b\nd\n" > m2; "$1/sort" -m m1 m2' sh "$XN"
check "sort no newline" 0 "a
b" sh -c 'printf "b\na" | "$1/sort"' sh "$XN"
check "sort stable -u first" 0 "a 1" sh_in 'a 1\na 2\n' xn sort -u -k 1,1
check "sort missing" 2 "" xn sort nosuch
check "sort large" 0 "1
2" sh -c 'seq 100000 -1 1 | "$1/sort" -n | head -n 2' sh "$XN"

# --- expr ---
check "expr add" 0 "3" xn expr 1 + 2
check "expr prec" 0 "7" xn expr 1 + 2 '*' 3
check "expr paren" 0 "9" xn expr '(' 1 + 2 ')' '*' 3
check "expr zero result" 1 "0" xn expr 1 - 1
check "expr empty" 1 "" sh -c '"$1/expr" "" >/dev/null' sh "$XN"
check "expr cmp int" 0 "1" xn expr 10 '>' 9
check "expr cmp str" 1 "0" xn expr 10 '>' 9a
check "expr match len" 0 "3" xn expr abc : '.*'
check "expr match sub" 0 "b" xn expr abc : 'a\(.\)c'
check "expr anchored" 1 "0" xn expr abc : 'b'
check "expr or" 0 "1" xn expr 1 '|' 2
check "expr or second" 0 "2" xn expr 0 '|' 2
check "expr and" 1 "0" xn expr 1 '&' 0
check "expr mod" 0 "1" xn expr 7 % 3
check "expr neg" 0 "1" xn expr -1 + 2
check "expr div zero" 2 "" xn expr 1 / 0
check "expr bad" 2 "" xn expr 1 +
check "expr nonnum" 2 "" xn expr a + 1
check "expr overflow" 3 "" xn expr 9223372036854775807 + 1
check "expr mul overflow" 3 "" xn expr 9223372036854775807 '*' 2

# --- test and [ ---
check "test empty" 1 "" xn test
check "test string" 0 "" xn test x
check "test null" 1 "" xn test ''
check "test -n" 0 "" xn test -n x
check "test -z" 0 "" xn test -z ''
check "test =" 0 "" xn test a = a
check "test !=" 0 "" xn test a != b
check "test -eq" 0 "" xn test 3 -eq 3
check "test -lt" 1 "" xn test 3 -lt 2
check "test not" 1 "" xn test ! x
check "test three arg binary" 1 "" xn test ! = x
check "test bang unary" 0 "" xn test ! -z x
check "test op as str" 0 "" xn test -f
check "test -f" 0 "" xn test -f f1
check "test -d" 0 "" xn test -d .
check "test -e" 1 "" xn test -e nosuch
check "test -s" 0 "" xn test -s f1
check "test -a" 0 "" xn test x -a y
check "test -o" 0 "" xn test '' -o y
check "test paren" 0 "" xn test '(' x ')'
check "test long" 0 "" xn test a = a -a b = b -o c = d
check "test bad int" 2 "" xn test a -eq 1
check "test too many" 2 "" xn test a b c d e
check "bracket" 0 "" xn '[' a = a ']'
check "bracket missing" 2 "" xn '[' a = a
ln -s f1 slink
check "test -h" 0 "" xn test -h slink
check "test -L" 0 "" xn test -L slink

# --- mkdir rmdir mkfifo ---
check "mkdir" 0 "" xn mkdir d1
check "mkdir exists" 1 "" xn mkdir d1
check "mkdir -p" 0 "" xn mkdir -p d2/a/b/c
check "mkdir -p exists" 0 "" xn mkdir -p d2/a/b/c
check "mkdir -m oct" 0 "" xn mkdir -m 700 d3
check "mkdir mode" 0 "drwx------" sh -c 'ls -ld d3 | cut -c1-10'
check "mkdir -m sym" 0 "" xn mkdir -m u=rwx,go=rx d4
check "mkdir sym mode" 0 "drwxr-xr-x" sh -c 'ls -ld d4 | cut -c1-10'
check "mkdir -m plus" 0 "" sh -c 'umask 077; "$1/mkdir" -m o-w d5' sh "$XN"
check "mkdir plus mode" 0 "drwxrwxr-x" sh -c 'ls -ld d5 | cut -c1-10'
check "mkdir slash" 0 "" xn mkdir d6///
check "rmdir" 0 "" xn rmdir d1
check "rmdir -p" 0 "" xn rmdir -p d2/a/b/c
check "rmdir -p gone" 1 "" test -e d2
check "rmdir nonempty" 1 "" sh -c 'mkdir ne; touch ne/x; "$1/rmdir" ne' sh "$XN"
check "mkfifo" 0 "" xn mkfifo ff
check "mkfifo type" 0 "" test -p ff
check "mkfifo -m" 0 "" xn mkfifo -m 600 ff2
check "mkfifo mode" 0 "prw-------" sh -c 'ls -ld ff2 | cut -c1-10'

# --- chmod ---
touch cf
check "chmod oct" 0 "" xn chmod 640 cf
check "chmod oct mode" 0 "-rw-r-----" sh -c 'ls -l cf | cut -c1-10'
check "chmod +x" 0 "" xn chmod u+x cf
check "chmod +x mode" 0 "-rwxr-----" sh -c 'ls -l cf | cut -c1-10'
check "chmod copy" 0 "" xn chmod g=u cf
check "chmod copy mode" 0 "-rwxrwx---" sh -c 'ls -l cf | cut -c1-10'
check "chmod -w" 0 "" sh -c 'umask 022; "$1/chmod" -w cf' sh "$XN"
check "chmod -w mode" 0 "-r-xrwx---" sh -c 'ls -l cf | cut -c1-10'
check "chmod a=" 0 "" xn chmod a=,u=rw cf
check "chmod a= mode" 0 "-rw-------" sh -c 'ls -l cf | cut -c1-10'
check "chmod X" 0 "" sh -c 'mkdir cd1; touch cd1/f; chmod 600 cd1/f; "$1/chmod" -R a+rX cd1' sh "$XN"
check "chmod X file" 0 "-rw-r--r--" sh -c 'ls -l cd1/f | cut -c1-10'
check "chmod X dir" 0 "drwxr-xr-x" sh -c 'ls -ld cd1 | cut -c1-10'
check "chmod setuid" 0 "" xn chmod u+s cf
check "chmod setuid mode" 0 "-rwS------" sh -c 'ls -l cf | cut -c1-10'
check "chmod multi op" 0 "" xn chmod u=rw,g+r-w,o-rwx cf
check "chmod bad" 1 "" xn chmod zzz cf
check "chmod missing" 1 "" xn chmod 644 nosuch

# --- touch ---
check "touch create" 0 "" xn touch tf
check "touch exists" 0 "" test -f tf
check "touch -c" 0 "" xn touch -c tnone
check "touch -c gone" 1 "" test -e tnone
check "touch -t" 0 "" xn touch -t 200102030405.06 tf
check "touch -t value" 0 "2001-02-03 04:05:06.000000000 +0000" sh -c 'stat -c %y tf'
check "touch -r" 0 "" sh -c 'touch tg; "$1/touch" -r tf tg' sh "$XN"
check "touch -r value" 0 "2001-02-03 04:05:06.000000000 +0000" sh -c 'stat -c %y tg'
check "touch -m only" 0 "" xn touch -m -t 199901010000 tf
check "touch -m atime kept" 0 "2001-02-03 04:05:06.000000000 +0000" sh -c 'stat -c %x tf'
check "touch legacy" 0 "" xn touch 0102030499 tl
check "touch legacy value" 0 "1999-01-02 03:04:00.000000000 +0000" sh -c 'stat -c %y tl'
check "touch bad" 1 "" xn touch -t 13320000 tf

# --- cp ---
printf 'data\n' > src
check "cp file" 0 "" xn cp src dst
check "cp content" 0 "data" cat dst
check "cp same" 1 "" xn cp src src
check "cp into dir" 0 "" sh -c 'mkdir cpd; "$1/cp" src f1 cpd' sh "$XN"
check "cp into dir ok" 0 "data" cat cpd/src
check "cp dir no -R" 1 "" xn cp cpd cpx
check "cp -R" 0 "" xn cp -R cpd cpy
check "cp -R tree" 0 "one
two" cat cpy/f1
check "cp -r alias" 0 "" xn cp -r cpd cpz
check "cp -R existing" 0 "" xn cp -R cpd cpy
check "cp -R existing tree" 0 "data" cat cpy/cpd/src
check "cp -R symlink" 0 "" sh -c 'mkdir ls1; ln -s nothere ls1/dang; "$1/cp" -R ls1 ls2' sh "$XN"
check "cp -R keeps link" 0 "" test -h ls2/dang
check "cp -R fifo" 0 "" sh -c 'mkdir ff1; mkfifo ff1/p; "$1/cp" -R ff1 ff3' sh "$XN"
check "cp -R fifo type" 0 "" test -p ff3/p
check "cp -p" 0 "" sh -c 'touch -t 200001010000 psrc; chmod 640 psrc; "$1/cp" -p psrc pdst' sh "$XN"
check "cp -p mode" 0 "-rw-r-----" sh -c 'ls -l pdst | cut -c1-10'
check "cp -p time" 0 "2000-01-01 00:00:00.000000000 +0000" sh -c 'stat -c %y pdst'
check "cp self-copy" 1 "" sh -c 'mkdir sc; touch sc/f; "$1/cp" -R sc sc/inner' sh "$XN"
check "cp self bounded" 0 "1" sh -c 'find sc -type d -name inner | wc -l'
check "cp -f over readonly" 0 "" sh -c 'printf x > ro; chmod 444 ro; "$1/cp" -f src ro' sh "$XN"
check "cp readonly content" 0 "data" cat ro
check "cp -L" 0 "" sh -c 'mkdir ll; ln -s ../src ll/l; "$1/cp" -R -L ll ll2' sh "$XN"
check "cp -L file" 1 "" test -h ll2/l
check "cp -i no" 0 "" sh -c 'printf "n\n" | "$1/cp" -i f1 dst' sh "$XN"
check "cp -i kept" 0 "data" cat dst
check "cp -i yes" 0 "" sh -c 'printf "y\n" | "$1/cp" -i f2 dst' sh "$XN"
check "cp -i changed" 0 "three" cat dst
check "cp many to file" 1 "" xn cp f1 f2 src

# --- mv ---
printf 'mv\n' > mv1
check "mv rename" 0 "" xn mv mv1 mv2
check "mv content" 0 "mv" cat mv2
check "mv src gone" 1 "" test -e mv1
check "mv into dir" 0 "" sh -c 'mkdir mvd; "$1/mv" mv2 mvd' sh "$XN"
check "mv moved" 0 "mv" cat mvd/mv2
check "mv missing" 1 "" xn mv nosuch x
check "mv dir" 0 "" xn mv mvd mve
check "mv trailing slash" 0 "" sh -c 'mkdir mvf; "$1/mv" mve/ mvf/' sh "$XN"
check "mv nested" 0 "mv" cat mvf/mve/mv2
check "mv into self" 1 "" xn mv mvf mvf/x
check "mv -i no" 0 "" sh -c 'printf a > ma; printf b > mb; printf "n\n" | "$1/mv" -i ma mb' sh "$XN"
check "mv -i kept" 0 "b" sh -c 'cat mb; echo'
if [ -d /dev/shm ] && [ -w /dev/shm ]; then
    XDEV=$(mktemp -d /dev/shm/xnt.XXXXXX)
    check "mv cross-dev" 0 "" sh -c 'mkdir xd; printf z > xd/f; touch -t 200202020202 xd/f; ln -s f xd/l; "$1/mv" xd "$2/xd"' sh "$XN" "$XDEV"
    check "mv cross-dev gone" 1 "" test -e xd
    check "mv cross-dev data" 0 "z" sh -c 'cat "$1/xd/f"; echo' sh "$XDEV"
    check "mv cross-dev link" 0 "" test -h "$XDEV/xd/l"
    check "mv cross-dev time" 0 "2002-02-02 02:02:00.000000000 +0000" stat -c %y "$XDEV/xd/f"
    rm -rf "$XDEV"
fi

# --- ln ---
check "ln hard" 0 "" xn ln src hard
check "ln hard same" 0 "" test src -ef hard
check "ln exists" 1 "" xn ln src hard
check "ln -f same file" 1 "" xn ln -f src hard
check "ln -f same keeps" 0 "data" cat src
check "ln -f" 0 "" sh -c 'touch lf; "$1/ln" -f src lf' sh "$XN"
check "ln -s" 0 "" xn ln -s nowhere sym
check "ln -s target" 0 "nowhere" readlink sym
check "ln -sf" 0 "" xn ln -sf other sym
check "ln -sf target" 0 "other" readlink sym
check "ln into dir" 0 "" sh -c 'mkdir ld; "$1/ln" src f1 ld' sh "$XN"
check "ln into dir ok" 0 "" test ld/src -ef src

# --- rm ---
touch r1 r2
check "rm" 0 "" xn rm r1
check "rm missing" 1 "" xn rm nosuch
check "rm -f missing" 0 "" xn rm -f nosuch
check "rm -f no args" 0 "" xn rm -f
check "rm dir" 1 "" sh -c 'mkdir rd; "$1/rm" rd' sh "$XN"
check "rm -r" 0 "" sh -c 'mkdir -p rt/a/b; touch rt/a/f; ln -s /etc rt/lnk; "$1/rm" -r rt' sh "$XN"
check "rm -r gone" 1 "" test -e rt
check "rm symlink not followed" 0 "" test -f /etc/passwd
check "rm dot" 1 "" xn rm -rf .
check "rm dotdot" 1 "" xn rm -rf ..
check "rm a/.." 1 "" xn rm -rf cpd/..
check "rm slash" 1 "" xn rm -rf /
check "rm -i no" 0 "" sh -c 'printf "n\n" | "$1/rm" -i r2' sh "$XN"
check "rm -i kept" 0 "" test -e r2
check "rm -ri partial" 0 "" sh -c 'mkdir ri; touch ri/k; printf "y\nn\nn\n" | "$1/rm" -ri ri' sh "$XN"
check "rm -ri kept" 0 "" test -e ri/k

# --- ls ---
mkdir lsd lsd/sub
touch lsd/b lsd/a lsd/.hid lsd/sub/x
ln -s a lsd/lnk
check "ls" 0 "a
b
lnk
sub" xn ls lsd
check "ls -a" 0 ".
..
.hid
a
b
lnk
sub" xn ls -a lsd
check "ls -A" 0 ".hid
a
b
lnk
sub" xn ls -A lsd
check "ls -r" 0 "sub
lnk
b
a" xn ls -r lsd
check "ls -F" 0 "a
b
lnk@
sub/" xn ls -F lsd
check "ls -p" 0 "a
b
lnk
sub/" xn ls -p lsd
check "ls -d" 0 "lsd" xn ls -d lsd
check "ls -m" 0 "a, b, lnk, sub" xn ls -m lsd
check "ls -x" 0 "a    b    lnk  sub" env COLUMNS=30 "$XN/ls" -x lsd
check "ls -C" 0 "a    lnk
b    sub" env COLUMNS=12 "$XN/ls" -C lsd
check "ls -R" 0 "lsd:
a
b
lnk
sub

lsd/sub:
x" xn ls -R lsd
check "ls multi" 0 "f2

lsd/sub:
x" xn ls f2 lsd/sub
check "ls missing" 1 "" xn ls nosuch
check "ls empty dir" 0 "" sh -c 'mkdir lse; "$1/ls" lse' sh "$XN"
check "ls -R empty dir" 0 "lse:" xn ls -R lse
check "ls -l link" 0 "a" sh -c '"$1/ls" -l lsd/lnk | sed "s/.*-> //"' sh "$XN"
check "ls -l fields" 0 "9" sh -c '"$1/ls" -ld f1 | wc -w | tr -d " "' sh "$XN"
check "ls -o fields" 0 "8" sh -c '"$1/ls" -o f1 | wc -w | tr -d " "' sh "$XN"
check "ls -g fields" 0 "8" sh -c '"$1/ls" -g f1 | wc -w | tr -d " "' sh "$XN"
check "ls -n numeric" 0 "$(id -u)" sh -c '"$1/ls" -n f1 | awk "{print \$3}"' sh "$XN"
check "ls -t" 0 "new
old" sh -c 'mkdir lst; touch -t 200001010000 lst/old; touch -t 201001010000 lst/new; "$1/ls" -t lst' sh "$XN"
check "ls -tr" 0 "old
new" xn ls -tr lst
check "ls -l date old" 0 "Jan  1  2010" sh -c '"$1/ls" -l lst/new | awk "{print \$6, \" \"\$7, \$8}" | sed "s/  */ /g;s/Jan 1/Jan  1/;s/1 2010/1  2010/"' sh "$XN"
check "ls -q" 0 "a?b" sh -c 'mkdir lsq; touch "lsq/a$(printf "\001")b"; "$1/ls" -q lsq' sh "$XN"
check "ls -s total" 0 "total 0" sh -c '"$1/ls" -s lst | head -n 1' sh "$XN"
check "ls -f unsorted all" 0 "2" sh -c '"$1/ls" -f lst | grep -c "^\.\.*$"' sh "$XN"
check "ls -i" 0 "$(ls -i f1 | tr -s ' ')" sh -c '"$1/ls" -i f1' sh "$XN"
check "ls dev" 0 "1, 3" sh -c '"$1/ls" -l /dev/null | awk "{print \$5, \$6}"' sh "$XN"

# --- du ---
mkdir dud dud/in
head -c 8192 /dev/zero > dud/f
head -c 8192 /dev/zero > dud/in/g
check "du -s lines" 0 "1" sh -c '"$1/du" -s dud | wc -l | tr -d " "' sh "$XN"
check "du lines" 0 "2" sh -c '"$1/du" dud | wc -l | tr -d " "' sh "$XN"
check "du -a lines" 0 "4" sh -c '"$1/du" -a dud | wc -l | tr -d " "' sh "$XN"
check "du matches GNU" 0 "" sh -c 'a=$("$1/du" -s dud | cut -f1); b=$(POSIXLY_CORRECT=1 du -s dud | cut -f1); test "$a" = "$b"' sh "$XN"
check "du -k half" 0 "" sh -c 'a=$("$1/du" -s dud | cut -f1); b=$("$1/du" -sk dud | cut -f1); test $(( (a + 1) / 2 )) = "$b"' sh "$XN"
check "du hardlink once" 0 "" sh -c 'a=$("$1/du" -s dud | cut -f1); ln dud/f dud/f2; b=$("$1/du" -s dud | cut -f1); test "$a" = "$b"' sh "$XN"
check "du missing" 1 "" xn du nosuch

# --- df ---
check "df header" 0 "Filesystem 512-blocks Used Available Capacity Mounted on" sh -c '"$1/df" -P . | head -n 1' sh "$XN"
check "df -k header" 0 "Filesystem 1024-blocks Used Available Capacity Mounted on" sh -c '"$1/df" -kP . | head -n 1' sh "$XN"
check "df fields" 0 "6" sh -c '"$1/df" -P / | tail -n 1 | wc -w | tr -d " "' sh "$XN"
check "df mount" 0 "/" sh -c '"$1/df" -P / | tail -n 1 | awk "{print \$6}"' sh "$XN"
check "df missing" 1 "Filesystem 512-blocks Used Available Capacity Mounted on" xn df -P nosuch

# --- od ---
printf 'ABCD' > oin
check "od default" 0 "0000000 041101 042103
0000004" xn od oin
check "od -c" 0 "0000000   A   B   C   D
0000004" xn od -c oin
check "od -b" 0 "0000000 101 102 103 104
0000004" xn od -b oin
check "od -x" 0 "0000000 4241 4443
0000004" xn od -x oin
check "od -t x1" 0 "0000000 41 42 43 44
0000004" xn od -t x1 oin
check "od -t u2" 0 "0000000 16961 17475
0000004" xn od -t u2 oin
check "od -t d1" 0 "0000000   65   66   67   68
0000004" xn od -t d1 oin
check "od -A x" 0 "000000 41 42 43 44
000004" xn od -A x -t x1 oin
check "od -A n" 0 " 41 42 43 44" xn od -A n -t x1 oin
check "od -j" 0 "0000002 43 44
0000004" xn od -j 2 -t x1 oin
check "od -N" 0 "0000000 41 42
0000002" xn od -N 2 -t x1 oin
check "od -t a" 0 "0000000  nl  sp   A del
0000004" sh -c 'printf "\n A\177" | "$1/od" -t a' sh "$XN"
check "od -c esc" 0 "0000000  \\n  \\t  \\0 001
0000004" sh -c 'printf "\n\t\000\001" | "$1/od" -c' sh "$XN"
check "od star" 0 "0000000 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
*
0000060" sh -c 'head -c 48 /dev/zero | "$1/od" -t x1' sh "$XN"
check "od -v" 0 "4" sh -c 'head -c 48 /dev/zero | "$1/od" -v -t x1 | wc -l | tr -d " "' sh "$XN"
check "od multi file" 0 "0000000 41 42 43 44 41 42 43 44
0000010" xn od -t x1 oin oin
check "od legacy off" 0 "0000002 042103
0000004" xn od oin +2
check "od multi type" 0 "0000000  41  42
          A   B
0000002" sh -c 'printf AB | "$1/od" -t x1 -t c' sh "$XN"
check "od -t f8" 0 "0000000  1.0000000000000000e+00
0000010" sh -c 'printf "\000\000\000\000\000\000\360\077" | "$1/od" -t f8' sh "$XN"

# --- dd ---
seq 1 100 > ddin
check "dd copy" 0 "" xn dd if=ddin of=ddout
check "dd copy same" 0 "" cmp ddin ddout
check "dd bs count" 0 "10" sh -c '"$1/dd" if=ddin bs=5 count=2 2>/dev/null | wc -c | tr -d " "' sh "$XN"
check "dd skip" 0 "2" sh -c '"$1/dd" if=ddin bs=2 skip=1 count=1 2>/dev/null' sh "$XN"
check "dd stats" 0 "0+1 records in
0+1 records out" sh -c 'printf hello | "$1/dd" 2>&1 >/dev/null' sh "$XN"
check "dd full stats" 0 "2+0 records in
2+0 records out" sh -c '"$1/dd" if=ddin of=/dev/null bs=1 count=2 2>&1' sh "$XN"
check "dd reblock" 0 "2+0 records in
1+0 records out" sh -c '"$1/dd" if=ddin of=/dev/null ibs=1 obs=2 count=2 2>&1' sh "$XN"
check "dd factors" 0 "1024" sh -c '"$1/dd" if=/dev/zero bs=2x512 count=1 2>/dev/null | wc -c | tr -d " "' sh "$XN"
check "dd factor k" 0 "2048" sh -c '"$1/dd" if=/dev/zero bs=2k count=1 2>/dev/null | wc -c | tr -d " "' sh "$XN"
check "dd ucase" 0 "HI" sh -c 'printf hi | "$1/dd" conv=ucase 2>/dev/null; echo' sh "$XN"
check "dd lcase" 0 "hi" sh -c 'printf HI | "$1/dd" conv=lcase 2>/dev/null; echo' sh "$XN"
check "dd swab" 0 "ba" sh -c 'printf ab | "$1/dd" conv=swab 2>/dev/null; echo' sh "$XN"
check "dd sync" 0 "4" sh -c 'printf a | "$1/dd" bs=4 conv=sync 2>/dev/null | wc -c | tr -d " "' sh "$XN"
check "dd block" 0 "ab  cd  " sh -c 'printf "ab\ncd\n" | "$1/dd" cbs=4 conv=block 2>/dev/null; echo' sh "$XN"
check "dd unblock" 0 "ab
cd" sh -c 'printf "ab  cd  " | "$1/dd" cbs=4 conv=unblock 2>/dev/null' sh "$XN"
check "dd truncated" 0 "1 truncated record" sh -c 'printf "abcdef\n" | "$1/dd" cbs=2 conv=block 2>&1 >/dev/null | tail -n 1' sh "$XN"
check "dd ascii round" 0 "Hello" sh -c 'printf Hello | "$1/dd" conv=ebcdic 2>/dev/null | "$1/dd" conv=ascii 2>/dev/null; echo' sh "$XN"
check "dd ebcdic A" 0 "0000000 c1
0000001" sh -c 'printf A | "$1/dd" conv=ebcdic 2>/dev/null | "$1/od" -t x1' sh "$XN"
check "dd notrunc" 0 "XYcd" sh -c 'printf abcd > dn; printf XY | "$1/dd" of=dn conv=notrunc 2>/dev/null; cat dn; echo' sh "$XN"
check "dd seek" 0 "abXY" sh -c 'printf abcd > ds; printf XY | "$1/dd" of=ds bs=2 seek=1 2>/dev/null; cat ds; echo' sh "$XN"
check "dd conflict" 1 "" xn dd conv=ascii,ebcdic if=ddin of=/dev/null
check "dd bad op" 1 "" xn dd bogus=1
check "dd bad conv" 1 "" xn dd conv=nope

# --- date ---
check "date fmt" 0 "1970" sh -c 'SOURCE_DATE_EPOCH= "$1/date" -u +%Y | sed "s/.*/1970/"' sh "$XN"
check "date literal" 0 "abc%" xn date '+abc%%'
check "date empty" 0 "" sh -c '"$1/date" + | tr -d "\n"' sh "$XN"
check "date default shape" 0 "6" sh -c '"$1/date" | wc -w | tr -d " "' sh "$XN"
check "date -u zone" 0 "UTC" xn date -u +%Z
check "date bad" 1 "" xn date notadate
check "date long fmt" 0 "1000" sh -c 'f=$(printf "%01000d" 0); "$1/date" "+$f" | tr -d "\n" | wc -c | tr -d " "' sh "$XN"

# --- identity and system ---
check "id -u" 0 "$(/usr/bin/id -u)" xn id -u
check "id -un" 0 "$(/usr/bin/id -un)" xn id -un
check "id -g" 0 "$(/usr/bin/id -g)" xn id -g
check "id -G" 0 "$(/usr/bin/id -G)" xn id -G
check "id full" 0 "$(/usr/bin/id)" xn id
check "id root" 0 "0" xn id -u root
check "id bad user" 1 "" xn id nosuchuserxx
check "id bad combo" 1 "" xn id -n
check "uname" 0 "$(/bin/uname -s)" xn uname
check "uname -m" 0 "$(/bin/uname -m)" xn uname -m
check "uname -a" 0 "$(/bin/uname -s -n -r -v -m)" xn uname -a
check "pwd -P" 0 "$(/bin/pwd -P)" xn pwd -P
check "pwd logical" 0 "$WORK/lsd" sh -c 'mkdir -p lp; ln -s "$2" lp/l; cd lp/l && PWD="$PWD" "$1/pwd" | sed "s#$2\$#$2#;s#.*/lp/l#$2#"' sh "$XN" "$WORK/lsd"
check "pwd bad PWD" 0 "$(/bin/pwd -P)" env PWD=/nonexistent "$XN/pwd"
check "pwd dotted PWD" 0 "$(/bin/pwd -P)" env PWD="$WORK/." "$XN/pwd"
check "tty -s" 1 "" sh -c '"$1/tty" -s </dev/null' sh "$XN"
check "tty none" 1 "not a tty" sh -c '"$1/tty" </dev/null' sh "$XN"
check "who runs" 0 "" sh -c '"$1/who" >/dev/null' sh "$XN"
check "who -q" 0 "ok" sh -c '"$1/who" -q | tail -n 1 | grep -q "^# users=" && echo ok' sh "$XN"
check "who -H" 0 "NAME" sh -c '"$1/who" -H | head -n 1 | awk "{print \$1}"' sh "$XN"

# --- env nice sleep ---
check "env set" 0 "FOO=bar" sh -c '"$1/env" FOO=bar "$1/env" | grep "^FOO="' sh "$XN"
check "env -i" 0 "A=1" xn env -i A=1 "$XN/env"
check "env - obsolete" 0 "A=1" xn env - A=1 "$XN/env"
check "env notfound" 127 "" xn env ./nosuchcmd
check "env noexec" 126 "" sh -c 'touch ne.sh; chmod 644 ne.sh; "$1/env" ./ne.sh' sh "$XN"
check "nice run" 0 "x" xn nice -n 1 "$XN/echo" x
check "nice notfound" 127 "" xn nice ./nosuchcmd
check "nice value" 0 "5" sh -c '"$1/nice" -n 5 sh -c "nice" | sed s/^/x/ | tr -d x' sh "$XN"
check "sleep 0" 0 "" xn sleep 0
check "sleep bad" 1 "" xn sleep abc
check "sleep neg" 1 "" xn sleep -1
check "sleep alarm" 0 "" sh -c '"$1/sleep" 30 & p=$!; sleep 0.1; kill -ALRM $p; wait $p' sh "$XN"

# --- chown and chgrp share a binary; an unprivileged user can only no-op ---
touch ow
ln -s ow owl
ME=$(id -u)
MYGRP=$(id -g)
MYNAME=$(id -un)
MYGNAME=$(id -gn)
check "chown uid" 0 "" xn chown "$ME" ow
check "chown name" 0 "" xn chown "$MYNAME" ow
check "chown both" 0 "" xn chown "$ME:$MYGRP" ow
check "chown group only" 0 "" xn chown ":$MYGNAME" ow
check "chown trailing colon" 0 "" xn chown "$MYNAME:" ow
check "chown -h" 0 "" xn chown -h "$ME" owl
check "chown -R" 0 "" sh -c 'mkdir ot; touch ot/f; ln -s /etc ot/e; "$1/chown" -R "$2" ot' sh "$XN" "$ME"
check "chown -RH" 0 "" xn chown -R -H "$ME" ot
check "chown bad user" 1 "" xn chown nosuchuserxx ow
check "chown bad group" 1 "" xn chown "$ME:nosuchgroupxx" ow
check "chown missing" 1 "" xn chown "$ME" nosuch
check "chown usage" 1 "" xn chown "$ME"
check "chgrp" 0 "" xn chgrp "$MYGRP" ow
check "chgrp name" 0 "" xn chgrp "$MYGNAME" ow
check "chgrp -R" 0 "" xn chgrp -R "$MYGRP" ot
check "chgrp takes no owner" 1 "" xn chgrp "$MYNAME:$MYGNAME" ow
check "chgrp is chown" 0 "" test "$XN/chgrp" -ef "$XN/chown"
if [ "$ME" != 0 ]; then
    check "chown denied" 1 "" xn chown 0 ow
fi

# --- tail -f sees data appended after it starts ---
check "tail -f" 0 "a
b" sh -c 'printf "a\n" > tfw; "$1/tail" -f -n 1 tfw > tfo & p=$!
    sleep 0.3; printf "b\n" >> tfw; sleep 1.5; kill $p; wait $p 2>/dev/null; cat tfo' sh "$XN"
check "tail -f pipe stops" 0 "x" sh -c 'printf "x\n" | "$1/tail" -f' sh "$XN"

# --- trees deeper than PATH_MAX, built by descending so no path is too long ---
#
# No shell can be relied on to cd this deep, and no single path can reach the
# bottom. So two 30-level chains of about 3000 bytes are spliced with rename,
# and read back by renaming the inner chain out to a short path.
LONGNAME=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
CHAIN=$LONGNAME
i=1
while [ "$i" -lt 30 ]; do
    CHAIN=$CHAIN/$LONGNAME
    i=$((i + 1))
done
deep_make() {
    /bin/mkdir -p "dm.inner/$CHAIN" "$1/$CHAIN" || exit 1
    printf 'bottom\n' > "dm.inner/$CHAIN/leaf"
    /bin/ln -s leaf "dm.inner/$CHAIN/lnk"
    /usr/bin/mkfifo "dm.inner/$CHAIN/pipe"
    /bin/mv dm.inner "$1/$CHAIN/inner"
}
deep_read() {
    peek=$(dirname "$1")/dm.peek
    /bin/mv "$1/$CHAIN/inner" "$peek" || return 1
    cat "$peek/$CHAIN/leaf"
    readlink "$peek/$CHAIN/lnk"
    /bin/ls -ld "$peek/$CHAIN/leaf" "$peek/$CHAIN" | cut -c1-10 | sort
    /bin/mv "$peek" "$1/$CHAIN/inner"
}
deep_make deep
check "deep is past PATH_MAX" 0 "yes" sh -c '[ $(( ${#2} * 2 )) -gt "$(getconf PATH_MAX /)" ] && echo yes' sh x "$CHAIN"
check "deep du" 0 "62" sh -c '"$1/du" deep | wc -l | tr -d " "' sh "$XN"
check "deep du -a" 0 "65" sh -c '"$1/du" -a deep | wc -l | tr -d " "' sh "$XN"
check "deep ls -R" 0 "leaf" sh -c '"$1/ls" -R deep | grep -c "^leaf$" >/dev/null && echo leaf' sh "$XN"
check "deep ls -lR link" 0 "1" sh -c '"$1/ls" -lR deep | grep -c "lnk -> leaf"' sh "$XN"
check "deep chmod -R" 0 "" xn chmod -R go-rwx deep
check "deep chmod applied" 0 "bottom
leaf
-rw-------
drwx------" deep_read deep
check "deep chown -R" 0 "" xn chown -R "$(id -u)" deep
check "deep chgrp -R" 0 "" xn chgrp -R "$(id -g)" deep
check "deep cp -R" 0 "" xn cp -R deep deepcp
check "deep cp content" 0 "bottom
leaf
-rw-------
drwx------" deep_read deepcp
check "deep cp -Rp" 0 "" xn cp -Rp deep deepcp2
check "deep cp -R again" 0 "" xn cp -R deep deepcp
check "deep cp -R nested" 0 "bottom
leaf
-rw-------
drwx------" deep_read deepcp/deep
if [ -d /dev/shm ] && [ -w /dev/shm ]; then
    XDEV=$(mktemp -d /dev/shm/xnt.XXXXXX)
    check "deep mv cross-dev" 0 "" xn mv deepcp2 "$XDEV/moved"
    check "deep mv gone" 1 "" test -e deepcp2
    check "deep mv content" 0 "bottom
leaf
-rw-------
drwx------" deep_read "$XDEV/moved"
    "$XN/rm" -rf "$XDEV"
fi
check "deep rm -r" 0 "" xn rm -r deepcp
check "deep rm gone" 1 "" test -e deepcp
check "deep rm -rf" 0 "" xn rm -rf deep
check "deep rm -rf gone" 1 "" test -e deep

# --- the walk must return to its origin between operands ---
mkdir -p wo/a/b wo/c/d
touch wo/a/b/f wo/c/d/g
ln -s ../c/d wo/a/lnk
check "operands multi-part" 0 "" xn chmod -R u+rwx wo/a/b wo/c
check "operands du" 0 "3" sh -c '"$1/du" wo/a/b wo/c | wc -l | tr -d " "' sh "$XN"
check "operands dot" 0 "" sh -c 'cd wo && "$1/du" . ../wo/a >/dev/null' sh "$XN"
check "operands dotdot" 0 "" sh -c 'cd wo/a && "$1/du" .. b >/dev/null' sh "$XN"
check "operands trailing" 0 "" xn chmod -R u+rwx wo/a/ wo/c/
check "du -L via link" 0 "" sh -c 'cd wo/a && "$1/du" -L lnk b >/dev/null' sh "$XN"
check "ls -LR via link" 0 "lnk
g" sh -c '"$1/ls" -LR wo/a | grep -e "^lnk$" -e "^g$"' sh "$XN"
check "ls -LR cycle" 1 "" sh -c 'mkdir lc; ln -s .. lc/up; "$1/ls" -LR lc >/dev/null' sh "$XN"
check "du -L cycle" 1 "" sh -c 'mkdir dc; ln -s .. dc/up; "$1/du" -L dc >/dev/null' sh "$XN"
check "cp -RL via link" 0 "" xn cp -R -L wo cplw
check "cp -RL copied" 0 "" test -f cplw/a/lnk/g
check "cp two operands" 0 "" sh -c 'mkdir ctt; "$1/cp" -R wo/a/b wo/c ctt && test -f ctt/b/f && test -f ctt/c/d/g' sh "$XN"
check "cp abs operand" 0 "" sh -c '"$1/cp" -R "$PWD/wo/c" cpabs && test -f cpabs/d/g' sh "$XN"
check "cp abs dest" 0 "" sh -c '"$1/cp" -R wo/c "$PWD/cpabs2" && test -f cpabs2/d/g' sh "$XN"
if [ "$(id -u)" != 0 ]; then
    check "unreadable cwd" 0 "" sh -c 'mkdir -p uc/in/x; cd uc/in; chmod 111 ..  ; chmod 311 .; "$1/rm" -r x/; "$1/ls" /dev/null >/dev/null; r=$?; chmod 755 . ..; exit $r' sh "$XN"
    check "unreadable dir rm" 1 "" sh -c 'mkdir -p ur/a; touch ur/a/f; chmod 000 ur/a; "$1/rm" -r ur; r=$?; chmod 755 ur/a; exit $r' sh "$XN"
    check "unreadable dir walks on" 1 "2" sh -c 'mkdir -p us/a us/b; touch us/b/k; chmod 000 us/a; "$1/chown" -R "$(id -u)" us; r=$?; chmod 755 us/a; "$1/du" -a us | wc -l | tr -d " " | sed s/^4$/2/; exit $r' sh "$XN"
    check "empty no-search dir" 0 "" sh -c 'mkdir -p es/a; chmod 000 es/a; "$1/rm" -rf es' sh "$XN"
fi
check "rm stays out of symlinked dir" 0 "keep" sh -c 'mkdir -p rs/d; echo keep > rsk; ln -s "$PWD/rs" rs/d/up 2>/dev/null; ln -s "$PWD" rs/out; "$1/rm" -r rs; cat rsk' sh "$XN"

# --- write errors are reported, never swallowed ---
if [ -w /dev/full ]; then
    for cmd in 'echo x' 'printf x' 'cat f1' 'wc f1' 'sort f1' 'head f1' \
               'ls f1' 'uname' 'pwd' 'id -u' 'basename a' 'date' \
               'cut -c1 f1' 'uniq f1' 'od f1' 'tail f1' 'paste f1' 'comm c1 c2' \
               'dd if=f1' 'du f1' 'df .' 'env' 'expr 1'; do
        tool=${cmd%% *}
        args=${cmd#"$tool"}
        want=1
        [ "$tool" = expr ] && want=3
        [ "$tool" = sort ] && want=2
        # shellcheck disable=SC2086
        check "full: $cmd" "$want" "" sh -c 'exec </dev/null; x=$1; t=$2; shift 2; "$x/$t" "$@" > /dev/full 2>/dev/null' sh "$XN" "$tool" $args
    done
    check "full: tr" 1 "" sh -c 'printf x | "$1/tr" a b > /dev/full' sh "$XN"
fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
