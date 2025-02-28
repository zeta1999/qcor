#include "expression_handler.hpp"
#include "exprtk.hpp"
#include "qasm3_visitor.hpp"
using symbol_table_t = exprtk::symbol_table<double>;
using expression_t = exprtk::expression<double>;
using parser_t = exprtk::parser<double>;

namespace qcor {
antlrcpp::Any qasm3_visitor::visitLoopStatement(
    qasm3Parser::LoopStatementContext* context) {
  auto location = get_location(builder, file_name, context);

  auto loop_signature = context->loopSignature();
  auto program_block = context->programBlock();

  if (auto membership_test = loop_signature->membershipTest()) {
    // this is a for loop
    auto idx_var_name = membership_test->Identifier()->getText();

    auto set_declaration = membership_test->setDeclaration();
    if (set_declaration->LBRACE()) {
      auto exp_list = set_declaration->expressionList();
      auto n_expr = exp_list->expression().size();

      auto allocation =
          allocate_1d_memory(location, n_expr, builder.getI64Type());

      // allocate i64 memref of size n_expr
      // for i in {1,2,3} -> affine.for i : 0 to 3 { element = load(memref, i) }
      int counter = 0;
      for (auto exp : exp_list->expression()) {
        qasm3_expression_generator exp_generator(builder, symbol_table,
                                                 file_name);
        exp_generator.visit(exp);
        auto value = exp_generator.current_value;

        mlir::Value pos = get_or_create_constant_integer_value(
            counter, location, builder.getI64Type(), symbol_table, builder);

        builder.create<mlir::StoreOp>(
            location, value, allocation,
            llvm::makeArrayRef(std::vector<mlir::Value>{pos}));

        counter++;
      }

      symbol_table.enter_new_scope();

      auto tmp = get_or_create_constant_index_value(0, location, 64,
                                                    symbol_table, builder);
      auto tmp2 = get_or_create_constant_index_value(0, location, 64,
                                                     symbol_table, builder);
      llvm::ArrayRef<mlir::Value> zero_index(tmp2);

      auto loop_var_memref = allocate_1d_memory_and_initialize(
          location, 1, builder.getI64Type(), std::vector<mlir::Value>{tmp},
          llvm::makeArrayRef(std::vector<mlir::Value>{tmp}));

      auto b_val = get_or_create_constant_index_value(n_expr, location, 64,
                                                      symbol_table, builder);
      auto c_val = get_or_create_constant_index_value(1, location, 64,
                                                      symbol_table, builder);

      // Strategy...

      // We need to create a header block to check that loop var is still valid
      // it will branch at the end to the body or the exit

      // Then we create the body block, it should branch to the incrementor
      // block

      // Then we create the incrementor block, it should branch back to header

      // Any downstream children that will create blocks will need to know what
      // the fallback block for them is, and it should be the incrementor block
      auto savept = builder.saveInsertionPoint();
      auto currRegion = builder.getBlock()->getParent();
      auto headerBlock = builder.createBlock(currRegion, currRegion->end());
      auto bodyBlock = builder.createBlock(currRegion, currRegion->end());
      auto incBlock = builder.createBlock(currRegion, currRegion->end());
      mlir::Block* exitBlock =
          builder.createBlock(currRegion, currRegion->end());
      builder.restoreInsertionPoint(savept);

      builder.create<mlir::BranchOp>(location, headerBlock);
      builder.setInsertionPointToStart(headerBlock);

      auto load =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);
      auto cmp = builder.create<mlir::CmpIOp>(
          location, mlir::CmpIPredicate::slt, load, b_val);
      builder.create<mlir::CondBranchOp>(location, cmp, bodyBlock, exitBlock);

      builder.setInsertionPointToStart(bodyBlock);
      // Load the loop variable from the memref allocation
      auto load2 =
          builder.create<mlir::LoadOp>(location, allocation, load.result());

      // Save the loaded value as the loop variable name
      symbol_table.add_symbol(idx_var_name, load2.result(), {}, true);

      current_loop_exit_block = exitBlock;

      current_loop_incrementor_block = incBlock;

      visitChildren(program_block);

      current_loop_header_block = nullptr;
      current_loop_exit_block = nullptr;

      builder.create<mlir::BranchOp>(location, incBlock);

      builder.setInsertionPointToStart(incBlock);
      auto load_inc =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);
      auto add = builder.create<mlir::AddIOp>(location, load_inc, c_val);

      builder.create<mlir::StoreOp>(
          location, add, loop_var_memref,
          llvm::makeArrayRef(std::vector<mlir::Value>{tmp2}));

      builder.create<mlir::BranchOp>(location, headerBlock);

      builder.setInsertionPointToStart(exitBlock);

      symbol_table.set_last_created_block(exitBlock);

      // Exit scope and restore insertion
      symbol_table.exit_scope();

    } else if (auto range = set_declaration->rangeDefinition()) {
      // this is a range definition
      //     rangeDefinition
      // : LBRACKET expression? COLON expression? ( COLON expression )? RBRACKET
      // ;
      auto is_constant = [](const std::string idx_str) {
        try {
          std::stoi(idx_str);
          return true;
        } catch (std::exception& ex) {
          return false;
        }
      };

      auto range_str =
          range->getText().substr(1, range->getText().length() - 2);
      auto range_elements = split(range_str, ':');
      auto n_expr = range->expression().size();
      int a, b, c;
      if (is_constant(range_elements[0])) {
        a = std::stoi(range_elements[0]);
      } else {
        // infer the constant from the symbol table
        a = symbol_table.evaluate_constant_integer_expression(
            range->expression(0)->getText());
      }

      c = 1;
      if (n_expr == 3) {
        if (is_constant(range_elements[2])) {
          b = std::stoi(range_elements[2]);
        } else {
          b = symbol_table.evaluate_constant_integer_expression(
              range->expression(2)->getText());
        }
        if (is_constant(range_elements[1])) {
          c = std::stoi(range_elements[1]);
        } else {
          c = symbol_table.evaluate_constant_integer_expression(
              range->expression(1)->getText());
        }
      } else {
        if (is_constant(range_elements[1])) {
          b = std::stoi(range_elements[1]);
        } else {
          b = symbol_table.evaluate_constant_integer_expression(
              range->expression(1)->getText());
        }
      }
      // std::cout << "A,b,c: " << a << ", " << b << ", " << c << "\n";

      if (a < 0) {
        printErrorMessage("first element of range must be >= 0.");
      }

      if (c < 1) {
        printErrorMessage("step element of range must be > 0.");
      }

      if (b < a) {
        printErrorMessage(
            "end value of range must be greater than start value.");
      }

      // Create a new scope for the for loop
      symbol_table.enter_new_scope();

      auto tmp = get_or_create_constant_integer_value(
          a, location, builder.getI64Type(), symbol_table, builder);
      auto tmp2 = get_or_create_constant_index_value(0, location, 64,
                                                     symbol_table, builder);
      llvm::ArrayRef<mlir::Value> zero_index(tmp2);

      auto loop_var_memref = allocate_1d_memory_and_initialize(
          location, 1, builder.getI64Type(), std::vector<mlir::Value>{tmp},
          llvm::makeArrayRef(std::vector<mlir::Value>{tmp2}));

      auto b_val = get_or_create_constant_integer_value(
          b, location, builder.getI64Type(), symbol_table, builder);
      auto c_val = get_or_create_constant_integer_value(
          c, location, builder.getI64Type(), symbol_table, builder);

      // Save the current builder point
      // auto savept = builder.saveInsertionPoint();
      auto loaded_var =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);

      symbol_table.add_symbol(idx_var_name, loaded_var, {}, true);

      // Strategy...

      // We need to create a header block to check that loop var is still valid
      // it will branch at the end to the body or the exit

      // Then we create the body block, it should branch to the incrementor
      // block

      // Then we create the incrementor block, it should branch back to header

      // Any downstream children that will create blocks will need to know what
      // the fallback block for them is, and it should be the incrementor block
      auto savept = builder.saveInsertionPoint();
      auto currRegion = builder.getBlock()->getParent();
      auto headerBlock = builder.createBlock(currRegion, currRegion->end());
      auto bodyBlock = builder.createBlock(currRegion, currRegion->end());
      auto incBlock = builder.createBlock(currRegion, currRegion->end());
      mlir::Block* exitBlock =
          builder.createBlock(currRegion, currRegion->end());
      builder.restoreInsertionPoint(savept);

      builder.create<mlir::BranchOp>(location, headerBlock);
      builder.setInsertionPointToStart(headerBlock);

      auto load =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);
      auto cmp = builder.create<mlir::CmpIOp>(
          location, mlir::CmpIPredicate::slt, load, b_val);
      builder.create<mlir::CondBranchOp>(location, cmp, bodyBlock, exitBlock);

      builder.setInsertionPointToStart(bodyBlock);
      // body needs to load the loop variable
      auto x =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);
      symbol_table.add_symbol(idx_var_name, x, {}, true);

      current_loop_exit_block = exitBlock;

      current_loop_incrementor_block = incBlock;

      visitChildren(program_block);

      current_loop_incrementor_block = nullptr;
      current_loop_exit_block = nullptr;

      builder.create<mlir::BranchOp>(location, incBlock);

      builder.setInsertionPointToStart(incBlock);
      auto load_inc =
          builder.create<mlir::LoadOp>(location, loop_var_memref, zero_index);
      auto add = builder.create<mlir::AddIOp>(location, load_inc, c_val);

      builder.create<mlir::StoreOp>(
          location, add, loop_var_memref,
          llvm::makeArrayRef(std::vector<mlir::Value>{tmp2}));

      builder.create<mlir::BranchOp>(location, headerBlock);

      builder.setInsertionPointToStart(exitBlock);

      symbol_table.set_last_created_block(exitBlock);

      symbol_table.exit_scope();

    } else {
      printErrorMessage(
          "For loops must be of form 'for i in {SET}' or 'for i in [RANGE]'.");
    }

  } else {
    // this is a while loop
    auto while_expr = loop_signature->booleanExpression();

    // Create a new scope for the for loop
    symbol_table.enter_new_scope();

    auto currRegion = builder.getBlock()->getParent();

    auto savept = builder.saveInsertionPoint();
    auto headerBlock = builder.createBlock(currRegion, currRegion->end());
    auto bodyBlock = builder.createBlock(currRegion, currRegion->end());
    auto exitBlock = builder.createBlock(currRegion, currRegion->end());

    builder.restoreInsertionPoint(savept);
    builder.create<mlir::BranchOp>(location, headerBlock);

    builder.setInsertionPointToEnd(headerBlock);
    qasm3_expression_generator exp_generator(builder, symbol_table, file_name);
    exp_generator.visit(while_expr);
    auto expr_value = exp_generator.current_value;
    builder.create<mlir::CondBranchOp>(location, expr_value, bodyBlock,
                                       exitBlock);

    builder.setInsertionPointToStart(bodyBlock);
    current_loop_exit_block = exitBlock;
    current_loop_header_block = headerBlock;

    visitChildren(program_block);

    current_loop_header_block = nullptr;
    current_loop_exit_block = nullptr;

    builder.create<mlir::BranchOp>(location, headerBlock);
    builder.setInsertionPointToStart(exitBlock);

    symbol_table.exit_scope();

    // This is where we do some manipulation of
    // the basic blocks, lets store the current last block
    // so that finalize_mlirgen() can add return and deallocs
    // correctly

    symbol_table.set_last_created_block(exitBlock);
  }

  return 0;
}

antlrcpp::Any qasm3_visitor::visitControlDirective(
    qasm3Parser::ControlDirectiveContext* context) {
  auto location = get_location(builder, file_name, context);

  auto stmt = context->getText();

  if (stmt == "break") {
    builder.create<mlir::BranchOp>(location, current_loop_exit_block);
  } else if (stmt == "continue") {
    if (current_loop_incrementor_block) {
      builder.create<mlir::BranchOp>(location, current_loop_incrementor_block);
    } else if (current_loop_header_block) {
      // this is a while loop
      builder.create<mlir::BranchOp>(location, current_loop_header_block);
    } else {
      printErrorMessage(
          "Something went wrong with continue, no valid block to branch to.");
    }
  } else {
    printErrorMessage("we do not yet support the " + stmt +
                      " control directive.");
  }

  return 0;
}
}  // namespace qcor