/*
    SlimeVR Code is placed under the MIT license
    Copyright (c) 2021 S.J. Remington & SlimeVR contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#include "LSM6DS3sensor.h"
#include "network/network.h"
#include "GlobalVars.h"
#include "calibration.h"
#include "network/network.h"

#define DISABLE_ASCALE true
#define MADGWICK false
#define FORCE_CALIBRATION false

void LSM6DS3Sensor::motionSetup()
{
    imu.initialize(addr);
#if FORCE_CALIBRATION
    startCalibration(0);
#endif
    if (!imu.testConnection())
    {
        m_Logger.fatal("Can't connect to LSM6DS3 (0x%02x) at address 0x%02x", imu.getDeviceID(), addr);
        return;
    }
    m_Logger.info("Connected to LSM6DS3 (0x%02x) at address 0x%02x", imu.getDeviceID(), addr);

    int16_t az;
    imu.getRawAccelZ(&az);
    float g_az = (float)(az);
    if (g_az < -0.75f)
    {
        m_Logger.info("Flip front to confirm start calibration");
        delay(5000);
        imu.getRawAccelZ(&az);
        if (g_az > 0.75f)
        {
            m_Logger.debug("Starting calibration...");
            startCalibration(0);
        }
    }
    {
        SlimeVR::Configuration::CalibrationConfig sensorCalibration = configuration.getCalibration(sensorId);
        switch (sensorCalibration.type)
        {
        case SlimeVR::Configuration::CalibrationConfigType::LSM6DS3:
            m_Calibration = sensorCalibration.data.lsm6ds3;
            break;
        case SlimeVR::Configuration::CalibrationConfigType::NONE:
            m_Logger.warn("No calibration data found for sensor %d, ignoring...", sensorId);
            m_Logger.info("Calibration is advised");
            break;
        default:
            m_Logger.warn("Incompatible calibration data found for sensor %d, ignoring...", sensorId);
            m_Logger.info("Calibration is advised");
        }
    }
    working = true;
}

void LSM6DS3Sensor::motionLoop()
{
#if ENABLE_INSPECTION
    {
        int16_t ax, ay, az, gx, gy, gz = 0;
        imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        Network::sendInspectionRawIMUData(sensorId, &gx, &gy, &gz, 255, &ax, &ay, &az, 255, 0, 0, 0, 255);
    }
#endif
    now = micros();
    deltat = now - last;
    last = now;
    float AGxyz[6] = {0};
    getScaledValues(AGxyz);
#if MADGWICK
    madgwickQuaternionUpdate(q, AGxyz[0], AGxyz[1], AGxyz[2], AGxyz[3], AGxyz[4], AGxyz[5], deltat * 1.0e-6f);
#else
    mahonyQuaternionUpdate(q, AGxyz[0], AGxyz[1], AGxyz[2], AGxyz[3], AGxyz[4], AGxyz[5], deltat * 1.0e-6f);
#endif
    quaternion.set(-q[2], q[1], q[3], q[0]);
    quaternion *= sensorOffset;
    Network::sendTemperature(imu.getTemperature(), sensorId);
#if ENABLE_INSPECTION
    {
        Network::sendInspectionFusedIMUData(sensorId, quaternion);
    }
#endif

    if (!OPTIMIZE_UPDATES || !lastQuatSent.equalsWithEpsilon(quaternion))
    {
        newData = true;
        lastQuatSent = quaternion;
    }
}

void LSM6DS3Sensor::getScaledValues(float AGxyz[6])
{
    int16_t ax, ay, az, gx, gy, gz = 0;
#if ENABLE_INSPECTION
    {
        imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        Network::sendInspectionRawIMUData(sensorId, &gx, &gy, &gz, 255, &ax, &ay, &az, 255, 0, 0, 0, 255);
    }
#endif
    imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
#if DISABLE_ASCALE
    AGxyz[0] = (float)ax;
    AGxyz[1] = (float)ay;
    AGxyz[2] = (float)az;
#else
    AGxyz[0] = (float)ax * 0.061 * (imu.settings.accelRange >> 1) / 1000;
    AGxyz[1] = (float)ay * 0.061 * (imu.settings.accelRange >> 1) / 1000;
    AGxyz[2] = (float)az * 0.061 * (imu.settings.accelRange >> 1) / 1000;
#endif
    AGxyz[3] = ((float)gx - (m_Calibration.G_off[0])) * imu.gscale;
    AGxyz[4] = ((float)gy - (m_Calibration.G_off[1])) * imu.gscale;
    AGxyz[5] = ((float)gz - (m_Calibration.G_off[2])) * imu.gscale;
}

void LSM6DS3Sensor::startCalibration(int calibrationType)
{
    ledManager.on();
    m_Logger.debug("Gathering raw data for device calibration...");
    constexpr int calibrationSamples = 300;
    // Reset values
    float Gxyz[3] = {0};
    // Wait for sensor to calm down before calibration
    m_Logger.info("Put down the device and wait for baseline gyro reading calibration");
    delay(2000);
    for (int i = 0; i < calibrationSamples; i++)
    {
        int16_t ax, ay, az, gx, gy, gz;
        imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        Gxyz[0] += float(gx);
        Gxyz[1] += float(gy);
        Gxyz[2] += float(gz);
    }
    Gxyz[0] /= calibrationSamples;
    Gxyz[1] /= calibrationSamples;
    Gxyz[2] /= calibrationSamples;
#ifdef DEBUG_SENSOR
    m_Logger.trace("Gyro calibration results: %f %f %f", Gxyz[0], Gxyz[1], Gxyz[2]);
#endif

    Network::sendRawCalibrationData(Gxyz, CALIBRATION_TYPE_EXTERNAL_GYRO, 0);
    m_Calibration.G_off[0] = Gxyz[0];
    m_Calibration.G_off[1] = Gxyz[1];
    m_Calibration.G_off[2] = Gxyz[2];
    // Blink calibrating led before user should rotate the sensor
    m_Logger.info("Gently rotate the device while it's gathering accelerometer data");
    ledManager.pattern(15, 300, 3000 / 310);
    float *calibrationDataAcc = (float *)malloc(calibrationSamples * 3 * sizeof(float));
    for (int i = 0; i < calibrationSamples; i++)
    {
        int16_t ax, ay, az, gx, gy, gz;
        imu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
        calibrationDataAcc[i * 3 + 0] = ax;
        calibrationDataAcc[i * 3 + 1] = ay;
        calibrationDataAcc[i * 3 + 2] = az;
        Network::sendRawCalibrationData(calibrationDataAcc, CALIBRATION_TYPE_EXTERNAL_ACCEL, 0);
        ledManager.off();
        delay(250);
    }
    m_Logger.debug("Calculating calibration data...");
    float A_BAinv[4][3];
    CalculateCalibration(calibrationDataAcc, calibrationSamples, A_BAinv);
    free(calibrationDataAcc);
    m_Logger.debug("Finished Calculate Calibration data");
    m_Logger.debug("Accelerometer calibration matrix:");
    m_Logger.debug("{");
    for (int i = 0; i < 3; i++)
    {
        m_Calibration.A_B[i] = A_BAinv[0][i];
        m_Calibration.A_Ainv[0][i] = A_BAinv[1][i];
        m_Calibration.A_Ainv[1][i] = A_BAinv[2][i];
        m_Calibration.A_Ainv[2][i] = A_BAinv[3][i];
        m_Logger.debug("  %f, %f, %f, %f", A_BAinv[0][i], A_BAinv[1][i], A_BAinv[2][i], A_BAinv[3][i]);
    }
    m_Logger.debug("}");
    m_Logger.debug("Saving the calibration data");
    SlimeVR::Configuration::CalibrationConfig calibration;
    calibration.type = SlimeVR::Configuration::CalibrationConfigType::LSM6DS3;
    calibration.data.lsm6ds3 = m_Calibration;
    configuration.setCalibration(sensorId, calibration);
    configuration.save();
    ledManager.off();
    Network::sendCalibrationFinished(CALIBRATION_TYPE_EXTERNAL_ALL, 0);
    m_Logger.debug("Saved the calibration data");
    m_Logger.info("Calibration data gathered");
}