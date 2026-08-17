package require ::quartus::project

if {![info exists ::env(FPGA_PART)]} {
    post_message -type error "Set FPGA_PART to the exact Stratix 10 device"
    qexit -error
}

set script_dir [file dirname [file normalize [info script]]]
set project_root [file dirname $script_dir]
set project_dir [file join $project_root build quartus]
file mkdir $project_dir

project_new [file join $project_dir retention_metadata_controller] -overwrite
set_global_assignment -name FAMILY "Stratix 10"
set_global_assignment -name DEVICE $::env(FPGA_PART)
set_global_assignment -name TOP_LEVEL_ENTITY retention_metadata_controller
set_global_assignment -name SYSTEMVERILOG_FILE \
    [file join $project_root rtl retention_metadata_controller.sv]
export_assignments
execute_flow -compile
project_close

