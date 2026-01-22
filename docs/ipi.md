ipi流程

发送端：

1. prepare_ipi_tx(target_cpu)：拿到目标CPU的ipi_data锁, i.e., ipi_data[target_cpu].lock

2. set_ipi_tx_arg(target_cpu, #i, #args)：设置参数#i为#args

3. start_ipi_tx(target_cpu, #ipi_vector)：发送IPI, 先设置data->vector = #ipi_vector, 再设置data->finish = 0标记为未完成, 最后发送IPI

4. wait_finish_ipi_tx(target_cpu)：等待目标CPU完成IPI（i.e.，data->finish == 1）。然后释放目标CPU的ipi_data锁

接收端

1. handle_ipi()：处理IPI，如果data->vector != 0，则调用arch_handle_ipi(data->vector)