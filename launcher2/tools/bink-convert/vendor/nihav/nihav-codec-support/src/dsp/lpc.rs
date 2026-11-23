//! LPC filter calculation.

/// Calculated LPC filter for the provided input data, filter order is defined by `filter` length.
pub fn calc_lpc_filter<T: Copy>(data: &[T], filter: &mut [f64])
where f64: From<T>
{
    let order = filter.len();

    let mut acorr = vec![0.0f64; order + 1];

    for (i, sum) in acorr.iter_mut().enumerate() {
        let src1 = &data[i..];
        *sum = data.iter().zip(src1.iter()).fold(0.0, |acc, (&a, &b)| acc + f64::from(a) * f64::from(b));
    }

    for el in filter.iter_mut() {
        *el = 0.0;
    }

    if acorr[0].abs() < 1.0e-6 {
        return;
    }

    let mut bwd0 = vec![0.0; order];
    let mut bwd1 = vec![0.0; order];

    bwd0[0] = 1.0 / acorr[0];
    filter[order - 1] = acorr[1] / acorr[0];

    for i in 2..=order {
        let eb = acorr[1..i].iter().zip(bwd0.iter()).fold(0.0, |acc, (&a, &b)| acc + a * b);

        let scale = 1.0 / (1.0 - eb * eb);
        for j in 0..i {
            bwd1[j] = if j > 0 { bwd0[j - 1] * scale } else { 0.0 };
            if j < i - 1 {
                bwd1[j] -= bwd0[i - 2 - j] * eb * scale;
            }
        }

        let ex = acorr[1..i].iter().rev().zip(filter.iter().rev()).fold(0.0, |acc, (&a, &b)| acc + a * b);
        let scale = acorr[i] - ex;
        for (filt, &add) in filter.iter_mut().rev().zip(bwd1.iter()).take(i) {
            *filt += add * scale;
        }

        std::mem::swap(&mut bwd0, &mut bwd1);
    }
}
