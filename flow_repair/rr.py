import random
import time
import numpy as np

def rand_sol(num_repair_chnk_round, rpr_chnk, rpr_sltns, SR, _ecK, _ecM, _rack_num, _sort_rack_index):
    print("====> enter RandSol:")
    
    # Initialize rpr_sltns to zeros
    for i in range(num_repair_chnk_round):
        for j in range(_rack_num):
            rpr_sltns[i][j] = 0
    
    # Random generate repair solutions
    random.seed(time.time())
    
    for i in range(num_repair_chnk_round):
        # For the last repair round
        if rpr_chnk[i] == -1:
            break
        print("----------------------------------")    
        # Random select a destination rack
        rpr_chnk_id = rpr_chnk[i]
        while True:
            des_rack_idx = random.randint(0, _rack_num - 1)
            des_rack = _sort_rack_index[rpr_chnk_id * _rack_num + des_rack_idx]
            # Use SR matrix to check available chunks
            if SR[i][des_rack] < _ecM:
                break
                
        print(f"des_rack_idx = {des_rack_idx}")
        print(f"des_rack = {des_rack}")
        
        # Select racks for reading data, the data in the destination rack will be read by default
        index = []
        for j in range(_rack_num):
            if j == des_rack_idx:
                continue
            index.append(j)
            
        print("可选的读取机架:")
        print(index)
        
        temp = len(index)
        cr_cnt = 0
        # Use SR matrix to get available chunks in destination rack
        read_cnt = SR[i][des_rack]
        
        while True:
            rand_idx = random.randint(0, temp - 1)
            read_rack_idx = index[rand_idx]
            
            # Use SR matrix to check if there are available chunks
            if SR[i][read_rack_idx] == 0:
                continue
                
            # Use SR matrix to get available chunks
            read_cnt += SR[i][read_rack_idx]
            read_rack = _sort_rack_index[rpr_chnk_id * _rack_num + read_rack_idx]
            rpr_sltns[i][read_rack] = 1
            cr_cnt += 1
            
            print(f"rand_idx = {rand_idx}, cr_cnt = {cr_cnt}, read_cnt = {read_cnt}")
            
            # Update index
            index[rand_idx] = index[temp - 1]
            print(index)
            temp -= 1
            print(f"temp = {temp}")
            
            if read_cnt >= _ecK:
                break
                
        # Update the solution
        rpr_sltns[i][des_rack] = -cr_cnt
        
    print("------after init: rpr_sltns:------")
    for row in rpr_sltns:
        print(row)
    
    # Calculate OUT and IN vectors
    OUT = [0] * _rack_num
    IN = [0] * _rack_num
    
    # Calculate OUT vector (sum of positive 1s per column)
    for j in range(_rack_num):
        for i in range(num_repair_chnk_round):
            if i < len(rpr_chnk) and rpr_chnk[i] == -1:
                break
            if rpr_sltns[i][j] == 1:
                OUT[j] += 1
    
    # Calculate IN vector (sum of absolute values of negatives per column)
    for j in range(_rack_num):
        for i in range(num_repair_chnk_round):
            if i < len(rpr_chnk) and rpr_chnk[i] == -1:
                break
            if rpr_sltns[i][j] < 0:
                IN[j] += abs(rpr_sltns[i][j])
    
    # Display OUT and IN vectors
    print("------IN vector:------")
    print(IN)

    print("------OUT vector:------")
    print(OUT)
    
    print(f"Maximum value: {max(max(OUT), max(IN))}")
    print(f"Sum: {sum(IN)}")
    
    return rpr_sltns, OUT, IN

# 使用示例
if __name__ == "__main__":
    # 参数设置
    _ecK = 10
    _ecM = 4
    
    # SR矩阵 - 每个条带在每个机架上的可用块数量
    SR = [
        [2,2,2,3,4],
        [1,2,2,4,4],
        [3,2,1,4,3],
        [1,4,3,4,1],
        [3,4,2,0,4],
        [3,2,3,1,4],
        [0,3,4,2,4],
        [0,4,4,1,4],
        [3,2,3,4,1],
        [3,2,3,4,1],
        [3,4,0,4,2],
        [2,4,4,3,0],
        [3,0,4,4,2],
        [3,4,1,4,1],
        [3,4,1,2,3],
        [3,3,4,1,2],
        [1,3,3,2,4],
        [3,2,0,4,4],
        [0,3,3,3,4],
        [3,1,2,4,3],
        [3,1,4,3,2],
        [3,2,1,4,3],
        [3,4,4,1,1],
        [3,1,1,4,4],
        [3,3,4,2,1]
    ]
    num_repair_chnk_round = len(SR)
    _rack_num = len(SR[0])

    # 初始化数组
    rpr_chnk = list(range(num_repair_chnk_round)) + [-1]
    rpr_sltns = [[0] * _rack_num for _ in range(num_repair_chnk_round)]
    
    # 排序机架索引数组
    _sort_rack_index = list(range(_rack_num)) * num_repair_chnk_round  # 简化示例
    
    # 调用函数
    start_time1 = time.time()
    rpr_sltns, OUT, IN= rand_sol(
        num_repair_chnk_round, rpr_chnk, rpr_sltns, SR, _ecK, _ecM, _rack_num, _sort_rack_index
    )
    end_time1 = time.time()
    time1 = end_time1 - start_time1

    print(f"总计算时间: {time1:.6f} 秒")