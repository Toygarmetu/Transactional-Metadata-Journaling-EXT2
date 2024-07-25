This program implements in EXT2 Filesystem:

mkdir: for creating an empty directory.      
rmdir: for removing an empty directory.     
rm: removing a regular file.    
ed: modifying a regular file.

Also implements Journaling on EXT2 to behave like an EXT3:    

The journal itself functions as a regular file, storing modified Metadata of inodes and blocks before they are written to the file system.  

So that we can have atomic-like operations on disk to prevent inconsistencies in case of any power failure. 

This approach allows for the addition of a journal to an ext2 file system without requiring any data conversion.
