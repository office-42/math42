% A MATLAB script, which math42 opens with File > Open or runs from a
% terminal as math42-calc examples/matlab-script.m
% Comments become (* ... *) on the way in, and % again on the way out.
A = [4 -2; 1 1]
det(A)
eig(A)
inv(A) . A
v = linspace(0, 1, 5);
sum(v)
x = [1, 2, ...
     3, 4]
s = 'strings in single quotes are read too'
t = 'an apostrophe in one is written it''s, and read that way'
B = A'
polyfit([1 2 3], [2 4 6], 1)
sortrows([3 1; 1 2; 2 5])
