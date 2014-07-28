# $Id: test_dryrun.rb 39544 2013-03-01 02:09:42Z drbrain $

require 'fileutils'
require 'test/unit'
require_relative 'visibility_tests'

class TestFileUtilsDryRun < Test::Unit::TestCase

  include FileUtils::DryRun
  include TestFileUtils::Visibility

  def setup
    super
    @fu_module = FileUtils::DryRun
  end

end
