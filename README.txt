compile and run commands

.\compiler.exe program.hyd
ml /nologo /c /coff program.asm
link /SUBSYSTEM:CONSOLE /MACHINE:X86 program.obj runtime.obj kernel32.lib /OUT:program.exe
program.exe

/lib
    *libs*
/data
    *variables*
/code
    \main
	*some code*
/end



in data section
var name size;
const name size value;
list name size lenght;

value
no suffix - decimal
b suffix - binary
o suffix - octal
h suffix - hex
'a' - char
list:index
list:start-end

<time>

in code section

math
set var value/var/const;
add var value/var/const;
sub var value/var/const;
mul var value/var/const;
div var value/var/const;
mod var value/var/const;

s_add var value/var/const;
s_sub var value/var/const;
s_mul var value/var/const;
s_div var value/var/const;
s_mod var value/var/const;

logic
and var value/var;
or var value/var;
xor var value/var;
not var;
bit var point bvalue;

lists
lst_set list lenght arg1 arg2...;
str_set list text;

print
debug var;

constructions
if boolvar:
*some code*
elseif: (optional)
*some code*
else: (optional)
*some code*
end;

for name(counter) value(start) value(end) bvalue(0=(+1),1=(-1)):
*some code*
end;

while boolvar:
*some code*
end;

libs
lib name alias;
<alias> func args;

<console>
input value/var/const mode;
output value/var/const mode;
sep value;
encode value; (set encode win1250+value)

mode
b - binary
o - octal
d - decimal
s - signed decimal
h - hex
c - char

<math>
abs var;
neg var;
pow var value/var/const;
root var value/var/const;
max value/var/const value/var/const var;
min value/var/const value/var/const var;

<random>
generate value/var/const value/var/const var;

<time>
date var var var; (gives year, month, day)
time var var var; (gives hours, mins, secs)
time+ var var var var; (gives hours, mins, secs, milsecs)
wait value; (secs)
wait+ value; (milsecs)
start timer;
pause timer;
resume timer;
stop timer;
take timer var; (secs)
take+ timer var; (milsecs)


#d list name size lenght;
#d clist name size args/text;
#d cont name size;

lst_set list lenght arg1 arg2...;
str_set list text;
lst_cmp list1 lenght arg1 arg2...;
str_cmp list text;
list:point
list:start-end