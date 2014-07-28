# Loads mkmf which is used to make makefiles for Ruby extensions  
require 'mkmf'  
  
# Give it a name  
extension_name = 'fsnet'  

dirs = ["/usr/local/lib"]

$libs += "-levent "
$libs += "-levent_extra "
$libs += "-levent_pthreads "
$libs += "-ljemalloc "


$CFLAGS += " -I/usr/include"
$CFLAGS += " -I/usr/local/include"
$CFLAGS += " -I/usr/local/include/jemalloc"

$LDFLAGS += " -L/usr/lib"
$LDFLAGS += " -L/usr/local/lib"


# The destination  
dir_config(extension_name)  
  
# Do the work  
create_makefile(extension_name)  