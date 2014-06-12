
require 'csv'

Dir[File.dirname(__FILE__) + '/src/manager/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/templete/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/*.rb'].each {|file| require file }
Dir.chdir(File.dirname(__FILE__));


Hero.reload_templete();
Item.reload_templete();
