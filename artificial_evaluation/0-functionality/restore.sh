#!/usr/bin/expect  

source ../config.exp

spawn $basedir/build/simulate.sh
expect "Welcome to ChCore shell!"
