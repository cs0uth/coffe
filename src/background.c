/*
 * This file is part of COFFE
 * Copyright (C) 2019 Goran Jelic-Cizmek
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <time.h>
#include <gsl/gsl_integration.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_odeiv.h>
#include <gsl/gsl_matrix.h>

#include "common.h"
#include "background.h"

struct integration_params
{
    double Omega0_cdm; /* omega parameter for cold dark matter */

    double Omega0_baryon; /* omega parameter for baryons */

    double Omega0_gamma; /* omega parameter for photons */

    double Omega0_de; /* present omega parameter for dark energy-like component */

    struct coffe_interpolation w; /* interpolator of w(z) */

    struct coffe_interpolation wint; /* result of exp(3*int((1 + w(z))/(1 + z))) */

    struct coffe_interpolation xint; /* result of Omega0_m/(1 - Omega0_m)*exp(-3*int(w(a)/a)) */
};


/* only needed here */

struct temp_background
{
    double *z; /* redshift */

    double *a; /* scale factor (normalized so that now a=1 */

    double *Hz; /* hubble parameter H(z) */

    double *conformal_Hz; /* conformal hubble parameter */

    double *conformal_Hz_prime; /* derivative of conformal hubble parameter wrt conformal time */

    double *D1; /* growth rate D_1(a) */

    double *D1_prime; /* derivative of D_1(a) wrt D1'(rphys)*rphys, dimensionless ('=d/drphys)*/

    double *f; /* growth function f=d(log D)/d(log a) */

    double *g; /* growth function g=(1+z)*D_1 */

    double *G1, *G2;

    double *comoving_distance; /* comoving distance (dimensionless) */

};


/**
    differential equation for the growth rate D_1
**/

static int growth_rate_ode(
    double a,
    const double y[],
    double f[],
    void *params
)
{
    struct integration_params *par = (struct integration_params *) params;
    double z = 1./a - 1;
    double w = interp_spline(&par->w, z);
    double x = interp_spline(&par->xint, z);

    f[0] = y[1];
    f[1] = -3./2*(1 - w/(1 + x))*y[1]/a
           +
            3./2*x/(1 + x)*y[0]/pow(a, 2);
    return GSL_SUCCESS;
}


/**
    jacobian for the growth rate differential equation
**/

static int growth_rate_jac(
    double a,
    const double y[],
    double *dfdy,
    double dfdt[],
    void *params
)
{
    struct integration_params *par = (struct integration_params *) params;
    gsl_matrix_view dfdy_mat =
        gsl_matrix_view_array(dfdy, 2, 2);
    double z = 1./a - 1;
    double w = interp_spline(&par->w, z);
    double x = interp_spline(&par->xint, z);
    double x_der =
        gsl_spline_eval_deriv(par->xint.spline, z, par->xint.accel);
    gsl_matrix *m = &dfdy_mat.matrix;
    gsl_matrix_set(m, 0, 0, 0.0);
    gsl_matrix_set(m, 0, 1, 1.0);
    gsl_matrix_set(m, 1, 0, 3./2./pow(a, 2)*(x/(1 + x)));
    gsl_matrix_set(m, 1, 1, -3./a/2./(1. - w/(1 + x)));
    dfdt[0] = 0.0;
    dfdt[1] =
        3*y[1]/pow(a, 2)/2./(1 - w/(1 + x))
       -3*y[0]/pow(a, 3)*(x/(1 + x))
       -3*y[0]/2./pow(a, 2)*(x*x_der/pow(1 + x, 2))
       +3*y[0]/2./pow(a, 2)*(x_der/(1 + x));
    return GSL_SUCCESS;
}


/**
    integrand of (1 + w)/(1 + z)
**/

static double integrand_w(
    double z,
    void *p
)
{
    struct integration_params *par = (struct integration_params *) p;
    return (1 + interp_spline(&par->w, z))/(1 + z);
}

/**
    integrand w(a)/a
**/

static double integrand_x(
    double a,
    void *p
)
{
    struct integration_params *par = (struct integration_params *) p;
    double result = interp_spline(&par->w, 1/a - 1)/a;
    return result;
}


static double integral_w(
    struct integration_params *par,
    double z
)
{
    double prec = 1E-5, result, error;
    gsl_integration_workspace *space =
        gsl_integration_workspace_alloc(COFFE_MAX_INTSPACE);

    gsl_function integrand;
    integrand.function = &integrand_w;
    integrand.params = par;

    gsl_integration_qag(
        &integrand, 0., z, 0, prec,
        COFFE_MAX_INTSPACE,
        GSL_INTEG_GAUSS61, space,
        &result, &error
    );
    gsl_integration_workspace_free(space);
    return exp(3*result);
}


static double integral_x(
    struct integration_params *par,
    double z
)
{
    double prec = 1E-5, result, error;
    gsl_integration_workspace *space =
        gsl_integration_workspace_alloc(COFFE_MAX_INTSPACE);

    gsl_function integrand;
    integrand.function = &integrand_x;
    integrand.params = par;

    gsl_integration_qag(
        &integrand, 1/(1 + z), 1., 0, prec,
        COFFE_MAX_INTSPACE,
        GSL_INTEG_GAUSS61, space,
        &result, &error
    );
    gsl_integration_workspace_free(space);
    return result;
}


/**
    integrand of the comoving distance
**/

static double integrand_comoving(
    double z,
    void *p
)
{
    struct integration_params *par = (struct integration_params *) p;
    double integrand = 1./pow(
        (par->Omega0_cdm + par->Omega0_baryon)*pow(1 + z, 3)
       +par->Omega0_gamma*pow(1 + z, 4)
       +par->Omega0_de*interp_spline(&par->wint, z),
        1./2);
    return integrand;
}

/**
    computes and stores all the background functions
**/

int coffe_background_init(
    struct coffe_parameters_t *par,
    struct coffe_background_t *bg
)
{
    clock_t start, end;
    printf("Initializing the background...\n");
    start = clock();

    gsl_error_handler_t *default_handler =
        gsl_set_error_handler_off();

    struct temp_background *temp_bg =
        (struct temp_background *)coffe_malloc(sizeof(struct temp_background));
    temp_bg->z = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->a = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->Hz = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->conformal_Hz = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->conformal_Hz_prime = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->D1 = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->g = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->f = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->G1 = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->G2 = (double *)coffe_malloc(sizeof(double)*par->background_bins);
    temp_bg->comoving_distance = (double *)coffe_malloc(sizeof(double)*par->background_bins);

    struct integration_params ipar;
    ipar.Omega0_cdm = par->Omega0_cdm;
    ipar.Omega0_baryon = par->Omega0_baryon;
    ipar.Omega0_gamma = par->Omega0_gamma;
    ipar.Omega0_de = par->Omega0_de;
    {
        double z, z_max = 100.;
        const size_t bins = 16384;
        double *z_array = (double *)coffe_malloc(sizeof(double)*(bins + 1));

        double *w_array = (double *)coffe_malloc(sizeof(double)*(bins + 1));
        for (size_t i = 0; i <= bins; ++i){
            z = z_max*i/(double)bins;
            z_array[i] = z;
            w_array[i] = common_wfunction(par, z);
        }
        init_spline(&ipar.w, z_array, w_array, bins + 1, 1);
        free(w_array);

        double *wint_array = (double *)coffe_malloc(sizeof(double)*(bins + 1));
        for (size_t i = 0; i <= bins; ++i){
            z = z_max*i/(double)bins;
            z_array[i] = z;
            wint_array[i] = integral_w(&ipar, z);
        }
        init_spline(&ipar.wint, z_array, wint_array, bins + 1, 1);
        free(wint_array);

        double *xint_array = (double *)coffe_malloc(sizeof(double)*(bins + 1));
        for (size_t i = 0; i <= bins; ++i){
            z = z_max*i/(double)bins;
            if (i != 0){
                z_array[i] = z;
                xint_array[i] = (ipar.Omega0_cdm + ipar.Omega0_baryon)/(1 - (ipar.Omega0_cdm + ipar.Omega0_baryon))*exp(-3*integral_x(&ipar, z));
            }
            else{
                z_array[i] = 0;
                xint_array[i] = (ipar.Omega0_cdm + ipar.Omega0_baryon)/(1 - (ipar.Omega0_cdm + ipar.Omega0_baryon));
            }
        }
        init_spline(&ipar.xint, z_array, xint_array, bins + 1, 1);
        free(xint_array);

        free(z_array);
    }

    double temp_comoving_result, temp_error;
    double a_initial, z, w, wint;
    gsl_odeiv_system sys =
        {growth_rate_ode, growth_rate_jac, 2, &ipar};

    const gsl_odeiv_step_type *step_type =
        gsl_odeiv_step_rk8pd;

    gsl_odeiv_step *step =
        gsl_odeiv_step_alloc(step_type, 2);
    gsl_odeiv_control *control =
        gsl_odeiv_control_y_new(1E-6, 0.0);
    gsl_odeiv_evolve *evolve =
        gsl_odeiv_evolve_alloc(2);

    gsl_integration_workspace *space =
        gsl_integration_workspace_alloc(COFFE_MAX_INTSPACE);

    gsl_function comoving_integral;
    comoving_integral.function = &integrand_comoving;
    comoving_integral.params = &ipar;

    /* initial values for the differential equation (D_1 and D_1') */
    double initial_values[2] = {0.05, 1.0};

    double h = 1E-6, prec = 1E-5;

    for (int i = 0; i<par->background_bins; ++i){
        a_initial = 0.05;
        initial_values[0] = 0.05;
        initial_values[1] = 1.0;

        z = 15.*i/(double)(par->background_bins - 1);

        w = interp_spline(&ipar.w, z);
        wint = interp_spline(&ipar.wint, z);

        (temp_bg->z)[i] = z;
        (temp_bg->a)[i] = 1./(1. + z);
        (temp_bg->Hz)[i] = sqrt(
             (par->Omega0_cdm + par->Omega0_baryon)*pow(1 + z, 3)
            +par->Omega0_gamma*pow(1 + z, 4)
            +par->Omega0_de*wint); // in units H0
        (temp_bg->conformal_Hz)[i] = (temp_bg->a)[i]*(temp_bg->Hz)[i]; // in units H0
        (temp_bg->conformal_Hz_prime)[i] = -(
            pow(1 + z, 3)*(2*(1 + z)*par->Omega0_gamma + (par->Omega0_cdm + par->Omega0_baryon))
           +(1 + 3*w)*par->Omega0_de*wint
        )/pow(1 + z, 2)/2.; // in units H0^2

        while (a_initial < (temp_bg->a)[i]){
            gsl_odeiv_evolve_apply(
                evolve, control, step,
                &sys, &a_initial, (temp_bg->a)[i],
                &h, initial_values
            );
        }

        (temp_bg->D1)[i] = initial_values[0];
        (temp_bg->g)[i] = (1 + z)*(temp_bg->D1)[i];
        (temp_bg->f)[i] = initial_values[1]*(temp_bg->a)[i]/(temp_bg->D1)[i];

        gsl_integration_qag(
            &comoving_integral, 0., z, 0, prec,
            COFFE_MAX_INTSPACE,
            GSL_INTEG_GAUSS61, space,
            &temp_comoving_result, &temp_error
        );

        (temp_bg->comoving_distance)[i] = temp_comoving_result; // dimensionless
        if (z > 1E-10){
            (temp_bg->G1)[i] =
                (temp_bg->conformal_Hz_prime)[i]
                    /pow((temp_bg->conformal_Hz)[i], 2)
                    +
                    (2 - 5*interp_spline(&par->magnification_bias1, z))
                   /(
                        (temp_bg->comoving_distance[i])
                       *(temp_bg->conformal_Hz)[i]
                    )
                    +
                    5*interp_spline(&par->magnification_bias1, z)
                    -
                    interp_spline(&par->evolution_bias1, z);

            (temp_bg->G2)[i] =
                (temp_bg->conformal_Hz_prime)[i]
                    /pow((temp_bg->conformal_Hz)[i], 2)
                    +
                    (2 - 5*interp_spline(&par->magnification_bias2, z))
                   /(
                        (temp_bg->comoving_distance[i])
                       *(temp_bg->conformal_Hz)[i]
                    )
                    +
                    5*interp_spline(&par->magnification_bias2, z)
                    -
                    interp_spline(&par->evolution_bias2, z);
        }
        else{
            (temp_bg->G1)[i] = 0;
            (temp_bg->G2)[i] = 0;
        }
    }

    /* initializing the splines; all splines are a function of z */
    init_spline(
        &bg->a,
        temp_bg->z,
        temp_bg->a,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->Hz,
        temp_bg->z,
        temp_bg->Hz,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->conformal_Hz,
        temp_bg->z,
        temp_bg->conformal_Hz,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->conformal_Hz_prime,
        temp_bg->z,
        temp_bg->conformal_Hz_prime,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->D1,
        temp_bg->z,
        temp_bg->D1,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->f,
        temp_bg->z,
        temp_bg->f,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->comoving_distance,
        temp_bg->z,
        temp_bg->comoving_distance,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->G1,
        temp_bg->z,
        temp_bg->G1,
        par->background_bins,
        par->interp_method
    );
    init_spline(
        &bg->G2,
        temp_bg->z,
        temp_bg->G2,
        par->background_bins,
        par->interp_method
    );

    /* inverse of the z, chi(z) spline (only one we need to invert) */
    init_spline(
        &bg->z_as_chi,
        temp_bg->comoving_distance,
        temp_bg->z,
        par->background_bins,
        par->interp_method
    );

    /* memory cleanup */
    free(temp_bg->z);
    free(temp_bg->a);
    free(temp_bg->Hz);
    free(temp_bg->conformal_Hz);
    free(temp_bg->conformal_Hz_prime);
    free(temp_bg->D1);
    free(temp_bg->g);
    free(temp_bg->f);
    free(temp_bg->G1);
    free(temp_bg->G2);
    free(temp_bg->comoving_distance);
    free(temp_bg);
    gsl_odeiv_step_free(step);
    gsl_odeiv_control_free(control);
    gsl_odeiv_evolve_free(evolve);
    gsl_integration_workspace_free(space);
    gsl_spline_free(ipar.w.spline), gsl_interp_accel_free(ipar.w.accel);
    gsl_spline_free(ipar.wint.spline), gsl_interp_accel_free(ipar.wint.accel);
    gsl_spline_free(ipar.xint.spline), gsl_interp_accel_free(ipar.xint.accel);

    gsl_set_error_handler(default_handler);

    end = clock();
    printf("Background initialized in %.2f s\n",
        (double)(end - start) / CLOCKS_PER_SEC);

    return EXIT_SUCCESS;
}

int coffe_background_free(
    struct coffe_background_t *bg
)
{
    free_spline(&bg->z_as_chi);
    free_spline(&bg->a);
    free_spline(&bg->Hz);
    free_spline(&bg->conformal_Hz);
    free_spline(&bg->conformal_Hz_prime);
    free_spline(&bg->D1);
    free_spline(&bg->g);
    free_spline(&bg->f);
    free_spline(&bg->G1);
    free_spline(&bg->G2);
    free_spline(&bg->comoving_distance);
    return EXIT_SUCCESS;
}
