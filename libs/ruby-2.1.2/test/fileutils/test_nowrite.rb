# $Id: test_nowrite.rb 39544 2013-03-01 02:09:42Z drbrain $

require 'fileutils'
require 'test/unit'
require_relative 'visibility_tests'

class TestFileUtilsNoWrite < Test::Unit::TestCase

  include FileUtils::NoWrite
  include TestFileUtils::Visibility

  def setup
    super
    @fu_module = FileUtils::NoWrite
  end

end
