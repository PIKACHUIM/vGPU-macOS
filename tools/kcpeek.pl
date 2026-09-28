#!/usr/bin/perl
# Dump printable context around a byte pattern inside a binary file.
#
# Built for kernel collections (`.kc`), where Info.plist personalities live as
# NUL-separated strings that macOS `strings` refuses to surface, and where
# `python3` is unavailable until Command Line Tools are installed.
#
# Usage:
#   perl kcpeek.pl AppleParavirtGPU
#   perl kcpeek.pl IOPCIMatch /System/Library/KernelCollections/BootKernelExtensions.kc 300 800

use strict;
use warnings;

my $needle = shift @ARGV;
unless (defined $needle) {
    die "usage: kcpeek.pl <pattern> [file] [before] [after]\n"
      . "  default file: /System/Library/KernelCollections/SystemKernelExtensions.kc\n";
}

my $path = shift @ARGV;
$path = '/System/Library/KernelCollections/SystemKernelExtensions.kc' unless defined $path;

my $before = shift @ARGV;
$before = 500 unless defined $before;

my $after = shift @ARGV;
$after = 1800 unless defined $after;

open my $fh, '<:raw', $path or die "cannot open $path: $!\n";
local $/;
my $data = <$fh>;
close $fh;

my $hits  = 0;
my $start = 0;
while (1) {
    my $at = index($data, $needle, $start);
    last if $at < 0;
    $hits++;
    my $from = $at - $before;
    $from = 0 if $from < 0;
    my $window = substr($data, $from, $before + length($needle) + $after);

    # Non-printables become newlines so each embedded string lands on its own line.
    $window =~ s/[^\x20-\x7e]/\n/g;
    my @lines = grep { /\S/ } split /\n/, $window;
    if (@lines) {
        print "===== hit #$hits \@ $at =====\n";
        print "$_\n" for @lines;
        print "\n";
    }
    $start = $at + 1;
}

print "===== $path: $hits hit(s) for '$needle' =====\n";
