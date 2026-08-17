`timescale 1ns/1ps

module tb_retention_metadata_controller;
    localparam int NUM_LINES = 16;
    localparam int ADDR_W = $clog2(NUM_LINES);

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic cmd_valid = 1'b0;
    logic cmd_ready;
    logic [2:0] cmd_op = '0;
    logic [ADDR_W-1:0] cmd_line_addr = '0;
    logic [1:0] cmd_class = '0;
    logic [31:0] current_epoch = '0;
    logic rsp_valid;
    logic rsp_hit;
    logic rsp_expired;
    logic [1:0] rsp_class;
    logic [31:0] rsp_age;

    always #5 clk = ~clk;

    retention_metadata_controller #(
        .NUM_LINES(NUM_LINES),
        .EPHEMERAL_RETENTION(32'd8),
        .EPOCH_RETENTION(32'd32)
    ) dut (
        .clk,
        .rst_n,
        .cmd_valid,
        .cmd_ready,
        .cmd_op,
        .cmd_line_addr,
        .cmd_class,
        .current_epoch,
        .rsp_valid,
        .rsp_hit,
        .rsp_expired,
        .rsp_class,
        .rsp_age
    );

    task automatic command(
        input logic [2:0] op,
        input logic [ADDR_W-1:0] address,
        input logic [1:0] retention_class
    );
        @(negedge clk);
        cmd_valid = 1'b1;
        cmd_op = op;
        cmd_line_addr = address;
        cmd_class = retention_class;
        @(negedge clk);
        cmd_valid = 1'b0;
    endtask

    initial begin
        repeat (2) @(negedge clk);
        rst_n = 1'b1;

        // Write line 3 into the ephemeral class.
        current_epoch = 32'd1;
        command(3'd1, 4'd3, 2'd0);
        if (!rsp_valid || !rsp_hit) $fatal(1, "write response failed");

        // It is valid before the 8-epoch deadline.
        current_epoch = 32'd7;
        command(3'd0, 4'd3, 2'd0);
        if (!rsp_valid || !rsp_hit || rsp_expired)
            $fatal(1, "live read failed");

        // It expires at age 8.
        current_epoch = 32'd9;
        command(3'd0, 4'd3, 2'd0);
        if (!rsp_valid || rsp_hit || !rsp_expired)
            $fatal(1, "expiry detection failed");

        // Re-write and migrate to the epoch class.
        current_epoch = 32'd10;
        command(3'd1, 4'd3, 2'd0);
        current_epoch = 32'd14;
        command(3'd3, 4'd3, 2'd1);
        if (!rsp_hit || rsp_class != 2'd1)
            $fatal(1, "migration failed");

        current_epoch = 32'd40;
        command(3'd0, 4'd3, 2'd0);
        if (!rsp_hit || rsp_expired)
            $fatal(1, "migrated line expired too early");

        $display("tb_retention_metadata_controller=PASS");
        $finish;
    end
endmodule

