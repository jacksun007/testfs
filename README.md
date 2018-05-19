Prerequisite
------------
popt: used for command line processing. Get it with the following command:

sudo apt-get install libpopt-dev

SWI-Prolog: used for invariant checking. Get it with the following command:

sudo apt-get install swi-prolog

If you decide to build it manually (their website: http://www.swi-prolog.org/),
you will need to create a symbolic link to the install path, such as:

mkdir /usr/lib/swi-prolog/
ln -s /usr/local/lib/swipl-5.10.4/include/ /usr/lib/swi-prolog/include/

Replace 5.10.4 with the actual version number of your build.

Compile
-------
Run "make" to make everything
Run "make [full|manual|no-recon|no-checking]" to make a specific build setup

full: includes Recon with Prolog checker, used for automated testing
manual: includes Recon with Prolog checker and logging, used for manual testing
no-recon: does not include Recon, used for automated testing
no-checking: includes Recon without Prolog checker, used for automated testing

Run
---
cp reference-disk.1 reference-disk
./testfs reference-disk

Adding Build Configuration
--------------------------
Copy an existing Makefile from the most relevant build to a new folder. In the
primary Makefile, you will need to add the appropriate entries to BUILD that is
the name of the new folder. Modify the new Makefile as necessary. The most 
likely changes will be different preprocessor defines and files. 


