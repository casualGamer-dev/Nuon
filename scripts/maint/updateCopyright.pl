#!/usr/bin/perl -i -w -p

@now = gmtime();

$NEWYEAR=$now[5]+1900;

s/Copyright([^-]*) (20[^-]*), The Nuon Project/Copyright$1 $2-${NEWYEAR}, The Nuon Project/;

s/Copyright(.*)-(20..), The Nuon Project/Copyright$1-${NEWYEAR}, The Nuon Project/;
