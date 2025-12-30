#pragma once

static void swap_long(long *a, long *b)
{
    long tmp = *a;
    *a = *b;
    *b = tmp;
}

static long partition(long *arr, long left, long right)
{
    long pivot = arr[(left + right) >> 1]; // 取中间值，降低最坏情况概率
    long i = left;
    long j = right;

    while (i <= j) {
        while (arr[i] < pivot)
            i++;
        while (arr[j] > pivot)
            j--;
        if (i <= j) {
            swap_long(&arr[i], &arr[j]);
            i++;
            j--;
        }
    }
    return i;
}

static void quick_sort(long *arr, long left, long right)
{
    if (left >= right)
        return;

    long idx = partition(arr, left, right);

    quick_sort(arr, left, idx - 1);
    quick_sort(arr, idx, right);
}

static void sort_long(long *arr, long n)
{
    if (!arr || n <= 1)
        return;
    quick_sort(arr, 0, n - 1);
}