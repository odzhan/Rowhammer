#!/bin/bash

for ((i=0; i<=2000; i++))
do 
	sudo ./attack -o 88
	
	echo "finish $i"
done
