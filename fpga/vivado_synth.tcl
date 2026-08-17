if {$argc < 1} {
    puts "usage: vivado -mode batch -source fpga/vivado_synth.tcl -tclargs <part>"
    exit 2
}

set part_name [lindex $argv 0]
set script_dir [file dirname [file normalize [info script]]]
set project_root [file dirname $script_dir]
set output_dir [file join $project_root build vivado]
file mkdir $output_dir

read_verilog -sv [file join $project_root rtl retention_metadata_controller.sv]
synth_design -top retention_metadata_controller -part $part_name
report_utilization -file [file join $output_dir utilization.txt]
report_timing_summary -file [file join $output_dir timing_summary.txt]
write_checkpoint -force [file join $output_dir retention_metadata_controller.dcp]

