module retention_metadata_controller #(
    parameter int NUM_LINES = 1024,
    parameter int ADDR_W = $clog2(NUM_LINES),
    parameter int EPOCH_W = 32,
    parameter logic [EPOCH_W-1:0] EPHEMERAL_RETENTION = 32'd128,
    parameter logic [EPOCH_W-1:0] EPOCH_RETENTION = 32'd4096
) (
    input  logic                 clk,
    input  logic                 rst_n,

    input  logic                 cmd_valid,
    output logic                 cmd_ready,
    input  logic [2:0]           cmd_op,
    input  logic [ADDR_W-1:0]    cmd_line_addr,
    input  logic [1:0]           cmd_class,
    input  logic [EPOCH_W-1:0]   current_epoch,

    output logic                 rsp_valid,
    output logic                 rsp_hit,
    output logic                 rsp_expired,
    output logic [1:0]           rsp_class,
    output logic [EPOCH_W-1:0]   rsp_age
);

    localparam logic [2:0] OP_READ       = 3'd0;
    localparam logic [2:0] OP_WRITE      = 3'd1;
    localparam logic [2:0] OP_REFRESH    = 3'd2;
    localparam logic [2:0] OP_MIGRATE    = 3'd3;
    localparam logic [2:0] OP_INVALIDATE = 3'd4;

    localparam logic [1:0] CLASS_EPHEMERAL = 2'd0;
    localparam logic [1:0] CLASS_EPOCH     = 2'd1;
    localparam logic [1:0] CLASS_DURABLE   = 2'd2;

    logic                   valid_mem [0:NUM_LINES-1];
    logic [1:0]             class_mem [0:NUM_LINES-1];
    logic [EPOCH_W-1:0]     last_write_mem [0:NUM_LINES-1];

    logic [EPOCH_W-1:0] observed_age;
    logic [EPOCH_W-1:0] observed_retention;
    logic observed_expired;
    integer reset_index;

    function automatic logic [EPOCH_W-1:0] retention_for(
        input logic [1:0] retention_class
    );
        case (retention_class)
            CLASS_EPHEMERAL: retention_for = EPHEMERAL_RETENTION;
            CLASS_EPOCH:     retention_for = EPOCH_RETENTION;
            default:         retention_for = {EPOCH_W{1'b1}};
        endcase
    endfunction

    always_comb begin
        cmd_ready = 1'b1;
        observed_age = current_epoch - last_write_mem[cmd_line_addr];
        observed_retention = retention_for(class_mem[cmd_line_addr]);
        observed_expired = valid_mem[cmd_line_addr] &&
                           (class_mem[cmd_line_addr] != CLASS_DURABLE) &&
                           (observed_age >= observed_retention);
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            rsp_valid <= 1'b0;
            rsp_hit <= 1'b0;
            rsp_expired <= 1'b0;
            rsp_class <= CLASS_EPHEMERAL;
            rsp_age <= '0;
            for (reset_index = 0; reset_index < NUM_LINES; reset_index++) begin
                valid_mem[reset_index] <= 1'b0;
            end
        end else begin
            rsp_valid <= cmd_valid && cmd_ready;
            rsp_hit <= 1'b0;
            rsp_expired <= 1'b0;

            if (cmd_valid && cmd_ready) begin
                rsp_class <= class_mem[cmd_line_addr];
                rsp_age <= observed_age;

                case (cmd_op)
                    OP_READ: begin
                        if (observed_expired) begin
                            valid_mem[cmd_line_addr] <= 1'b0;
                            rsp_expired <= 1'b1;
                        end else begin
                            rsp_hit <= valid_mem[cmd_line_addr];
                        end
                    end

                    OP_WRITE: begin
                        valid_mem[cmd_line_addr] <= 1'b1;
                        class_mem[cmd_line_addr] <= cmd_class;
                        last_write_mem[cmd_line_addr] <= current_epoch;
                        rsp_hit <= 1'b1;
                        rsp_class <= cmd_class;
                        rsp_age <= '0;
                    end

                    OP_REFRESH: begin
                        if (observed_expired) begin
                            valid_mem[cmd_line_addr] <= 1'b0;
                            rsp_expired <= 1'b1;
                        end else if (valid_mem[cmd_line_addr]) begin
                            last_write_mem[cmd_line_addr] <= current_epoch;
                            rsp_hit <= 1'b1;
                            rsp_age <= '0;
                        end
                    end

                    OP_MIGRATE: begin
                        if (observed_expired) begin
                            valid_mem[cmd_line_addr] <= 1'b0;
                            rsp_expired <= 1'b1;
                        end else if (valid_mem[cmd_line_addr]) begin
                            class_mem[cmd_line_addr] <= cmd_class;
                            last_write_mem[cmd_line_addr] <= current_epoch;
                            rsp_hit <= 1'b1;
                            rsp_class <= cmd_class;
                            rsp_age <= '0;
                        end
                    end

                    OP_INVALIDATE: begin
                        rsp_hit <= valid_mem[cmd_line_addr];
                        valid_mem[cmd_line_addr] <= 1'b0;
                    end

                    default: begin
                        rsp_hit <= 1'b0;
                    end
                endcase
            end
        end
    end

endmodule

