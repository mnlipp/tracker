#!/bin/bash

if [[ ! -e ../../utils/data-generators/cc/generate ]]; then
    echo "Run this from test/functional-tests/"
    exit 2
fi

# If we don't have ttl files, bring them from the data-generators
if [[ ! -d ttl ]]; then
   CURRENT=`pwd`
   cd ../../utils/data-generators/cc
   # Generation takes time. Don't ask if they are already generated.
   #  Checking just one random .ttl file... not good
   if [[ ! -e ttl/032-nmo_Email.ttl ]]; then
       ./generate max.cfg ;
   else
     echo "TTL directory already exist"
   fi
   cd $CURRENT
   echo "Moving ttl generated files to functional-tests folder"
   cp -R ../../utils/data-generators/cc/ttl . || (echo "Error generating ttl files" && exit 3)
else
   echo "TTL files already in place"
fi

echo "Killing tracker-processes"
tracker-control -k
# Give time to dbus to free the name!
sleep 2 


echo "Setting temporal directories for the DBs"
if [[ -e /tmp/xdg-data-home ]]; then
    rm -rf /tmp/xdg-data-home
fi
mkdir /tmp/xdg-data-home
export XDG_DATA_HOME=/tmp/xdg-data-home/

if [[ -e /tmp/xdg-cache-home ]]; then
    rm -rf /tmp/xdg-cache-home
fi
mkdir /tmp/xdg-cache-home
export XDG_CACHE_HOME=/tmp/xdg-cache-home/
