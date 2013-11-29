#!/bin/sh

SYNTAX="$1.syntax"
INPUT="$1.cli"
OUTPUT="$1.tmp"
EXPECTED="$1.out"
DIFF="$1.diff"
PERM=

if [ -f "$1.perm" ]
then
    PERM="-p $1.perm"
fi

if [ -f "$1.norm" ]
then
  ../src/recli -s $SYNTAX -qX syntax > $1.tmp 2>&1
  if [ "$?" != "0" ]
  then
     echo "FAILED syntax parser: $1"
     echo $1 >> .failed
     exit 1
  fi
  diff -w $1.norm $1.tmp 2>&1 > $DIFF
  if [ "$?" != "0" ]
  then
     echo "FAILED syntax normal form: $1"
     echo $1 >> .failed
     exit 1
  fi    
fi

../src/recli -s $SYNTAX $PERM < $INPUT > $OUTPUT 2>&1
if [ "$?" != "0" ]
then
   echo "FAILED running CLI: $1"
   exit 1
fi

diff $OUTPUT $EXPECTED 2>&1 > $DIFF
if [ "$?" = "0" ]
then
    rm -f $OUTPUT $DIFF
else
    echo "FAILED output diff: $1"
    echo $1 >> .failed
    exit 1
fi
echo "Success: $1"
