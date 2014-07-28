# $Id: test_verbose.rb 39544 2013-03-01 02:09:42Z drbrain $

require 'test/unit'
require 'fileutils'
require_relative 'visibility_tests'

class TestFileUtilsVerbose < Test::Unit::TestCase

  include FileUtils::Verbose
  include TestFileUtils::Visibility

  def setup
    super
    @fu_module = FileUtils::Verbose
  end

end
