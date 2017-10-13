#!/usr/bin/env ruby

require 'forwardable'

module Releasing
  module Utils
    def replace_pattern(filepath, pattern, replacement)
      content = File.open(filepath) { |f| f.read.gsub pattern, replacement }
      IO.write(filepath, content)
    end

    def prepend_line(filepath, line)
      content = IO.read filepath
      IO.write filepath, line + "\n" + content
    end

    def replace_first_line(filepath, new_first_line)
      lines = File.readlines filepath
      lines[0] = new_first_line
      File.open(filepath, 'w') do |f|
        lines.each { |line| f.puts line }
      end
    end

    def write_lines(filepath, lines)
      File.open(filepath, 'w') do |f|
        lines.each { |line| f.puts line }
      end
    end
  end

  class Version
    attr_reader :version, :release_date

    def initialize(version)
      @version = version
      @release_date = Time.now
    end

    def final_version
      @version.split("-")[0]
    end

    def format_release_date
      @release_date.strftime "%b %d, %Y"
    end

    def final?
      not @version.include? "-"
    end
  end

  class VersionSetter
    include Utils
    extend Forwardable

    def initialize(version)
      @version_inst = Version.new(version)
    end

    def_delegators :@version_inst, :version, :release_date, :final_version, :format_release_date, :final?

    def update_pbzip2_cpp
      unstable_comment = final? ? "" : " (unstable development revision)"
      finish = '\n");'

      replace_pattern 'pbzip2.cpp',
        /(Parallel BZIP2 v).*/,
        "\\1#{version} [#{format_release_date}]#{unstable_comment}#{finish}"
    end

    def spec_changelog_entry
      date_comment = final? ? "#{release_date.strftime('%a %b %d %Y')}" : "Wday Mon dd yyyy (NOT RELEASED YET)"
      author = "Jeff Gilchrist <pbzip2@compression.ca>"

      ["* #{date_comment} #{author} - #{final_version}-1",
       "- Release #{final_version}",
       ""]
    end

    def update_pbzip2_spec
      filepath = 'pbzip2.spec'
      lines = File.readlines filepath
      changelog_index = lines.index("%changelog\n")

      skip_write = false
      if not lines[changelog_index + 1].include?("#{final_version}-1")
        spec_changelog_entry.reverse_each { |line| lines.insert(changelog_index + 1, line) }
      elsif final? and lines[changelog_index + 1].include?("NOT RELEASED YET")
        lines[changelog_index + 1] = spec_changelog_entry[0]
      else
        skip_write = true
      end
      write_lines(filepath, lines) unless skip_write
    end

    def update_release_dates
      if final?
        %w(ChangeLog README COPYING).each do |f|
          replace_pattern f,
            /Mon dd, yyyy \(NOT RELEASED YET\)/,
            "#{format_release_date}"
        end
      else
        unreleased_date_comment = 'Mon dd, yyyy (NOT RELEASED YET)'
        prepend_line 'ChangeLog',
                     "Changes in #{final_version} (#{unreleased_date_comment})"
        replace_first_line 'README', unreleased_date_comment
        replace_pattern 'COPYING',
                        /^(pbzip2 version).*$/,
                        "\\1 #{final_version} of #{unreleased_date_comment}"
      end
    end

    def set_version
      update_pbzip2_cpp
      update_pbzip2_spec
      update_release_dates
    end
  end
end

vs = Releasing::VersionSetter.new(ARGV[0])
vs.set_version
