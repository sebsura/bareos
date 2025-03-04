#!/usr/bin/env python3
#   BAREOS - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2018-2025 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.


# -*- coding: utf-8 -*-

# selenium.common.exceptions.ElementNotInteractableException: requires >= selenium-3.4.0

from datetime import datetime
import logging
import os
import sys
from time import sleep
import unittest
import tempfile

from selenium import webdriver
from selenium.common.exceptions import *
from selenium.webdriver.common.by import By
from selenium.webdriver.common.desired_capabilities import DesiredCapabilities
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.support.ui import Select, WebDriverWait
from selenium.webdriver.chrome.service import Service as ChromeService


class BadJobException(Exception):
    """Raise when a started job doesn't result in ID"""

    def __init__(self, msg=None):
        msg = "Job ID could not be saved after starting the job."
        super(BadJobException, self).__init__(msg)


class ClientStatusException(Exception):
    """Raise when a client does not have the expected status"""

    def __init__(self, client, status, msg=None):
        if status == "enabled":
            msg = "%s is enabled and cannot be enabled again." % client
        if status == "disabled":
            msg = "%s is disabled and cannot be disabled again." % client
        super(ClientStatusException, self).__init__(msg)


class ClientNotFoundException(Exception):
    """Raise when the expected client is not found"""

    def __init__(self, client, msg=None):
        msg = "The client %s was not found." % client
        super(ClientNotFoundException, self).__init__(msg)


class ElementCoveredException(Exception):
    """Raise when an element is covered by something"""

    def __init__(self, value):
        msg = "Click on element %s failed as it was covered by another element." % value
        super(ElementCoveredException, self).__init__(msg)


class ElementTimeoutException(Exception):
    """Raise when waiting on an element times out"""

    def __init__(self, value):
        if value != "spinner":
            msg = "Waiting for element %s returned a TimeoutException." % value
        else:
            msg = (
                "Waiting for the spinner to disappear returned a TimeoutException."
                % value
            )
        super(ElementTimeoutException, self).__init__(msg)


class ElementNotFoundException(Exception):
    """Raise when an element is not found"""

    def __init__(self, value):
        msg = "Element %s was not found." % value
        super(ElementNotFoundException, self).__init__(msg)


class FailedClickException(Exception):
    """Raise when wait_and_click fails"""

    def __init__(self, value):
        msg = "Waiting and trying to click %s failed." % value
        super(FailedClickException, self).__init__(msg)


class LocaleException(Exception):
    """Raise when wait_and_click fails"""

    def __init__(self, dirCounter, langCounter):
        if dirCounter != langCounter:
            msg = (
                "The available languages in login did not meet expectations.\n Expected "
                + str(dirCounter)
                + " languages but got "
                + str(langCounter)
                + "."
            )
        else:
            msg = "The available languages in login did not meet expectations.\n"
        super(LocaleException, self).__init__(msg)


class WrongCredentialsException(Exception):
    """Raise when wait_and_click fails"""

    def __init__(self, username, password):
        msg = 'Username "%s" or password "%s" is wrong.' % (username, password)
        super(WrongCredentialsException, self).__init__(msg)


class SeleniumTest(unittest.TestCase):

    chrome_user_data_dir = tempfile.TemporaryDirectory()
    browser = "chrome"
    # Used by Univention AppCenter test: 1200x800
    # Large resolution to show website without hamburger menu, e.g. 1920x1080
    resolution_x = 1920
    resolution_y = 1080
    chromedriverpath = None
    base_url = "http://127.0.0.1/bareos-webui"
    username = "admin"
    password = "secret"
    profile = "admin"
    client = "bareos-fd"
    restorefile = "/usr/sbin/bconsole"
    # path to store logging files
    logpath = os.getcwd()
    # slow down test for debugging
    sleeptime = 0.0
    # max seconds to wait for an element
    maxwait = 5
    # time to wait before trying again
    # Value must be > 0.1.
    waittime = 0.2

    def setUp(self):
        # Configure the logger, for information about the timings set it to INFO
        # Selenium driver itself will write additional debug messages when set to DEBUG
        logging.basicConfig(
            filename="%s/webui-selenium-test.log" % (self.logpath),
            format="%(levelname)s %(module)s.%(funcName)s: %(message)s",
            level=logging.INFO,
        )
        self.logger = logging.getLogger()

        if self.browser == "chrome":
            self.chromedriverpath = self.getChromedriverpath()
            # chrome webdriver option: disable experimental feature
            opt = webdriver.ChromeOptions()
            # chrome webdriver option: specify user data directory
            opt.add_argument("--user-data-dir=" + self.chrome_user_data_dir.name)
            # Set some options to improve reliability
            # https://stackoverflow.com/a/55307841/11755457
            opt.add_argument("--disable-extensions")
            opt.add_argument("--dns-prefetch-disable")
            opt.add_argument("--disable-gpu")

            # test in headless mode?
            if self.chromeheadless:
                opt.add_argument("--headless")
                opt.add_argument("--no-sandbox")

            try:
                # selenium >= 4
                self.driver = webdriver.Chrome(
                    service=ChromeService(self.chromedriverpath), options=opt
                )
            except TypeError:
                # fallback to old selenium initialization.
                self.driver = webdriver.Chrome(self.chromedriverpath, options=opt)

        elif self.browser == "firefox":
            d = DesiredCapabilities.FIREFOX
            d["loggingPrefs"] = {"browser": "ALL"}
            fp = webdriver.FirefoxProfile()
            fp.set_preference(
                "webdriver.log.file", self.logpath + "/firefox_console.log"
            )
            self.driver = webdriver.Firefox(capabilities=d, firefox_profile=fp)
        else:
            raise RuntimeError("Browser {} not found.".format(str(self.browser)))
        #
        # set explicit window size
        #
        self.driver.set_window_size(self.resolution_x, self.resolution_y)
        # Large resolution to show website without hamburger menu.
        # self.driver.set_window_size(1920, 1080)

        # used as timeout for selenium.webdriver.support.expected_conditions (EC)
        self.wait = WebDriverWait(self.driver, self.maxwait)

        # take base url, but remove last /
        self.base_url = self.base_url.rstrip("/")
        self.verificationErrors = []
        self.logger.info("===================== TESTING =====================")

    #
    # Tests
    #
    def disabled_test_login(self):
        self.login()
        self.logout()

    def test_client_link_on_dashboard(self):
        self.login()
        self.select_navbar_element("dashboard")
        self.wait_and_click(By.LINK_TEXT, self.client)
        self.logout()

    def test_client_disabling(self):
        # This test navigates to clients, ensures client is enabled,
        # disables it, closes a possible modal, goes to dashboard and reenables client.
        self.login()
        # Clicks on client menue tab
        self.select_navbar_element("client")
        # Tries to click on client...
        try:
            self.wait_and_click(By.LINK_TEXT, self.client)
        # Raises exception if client not found
        except ElementTimeoutException:
            raise ClientNotFoundException(self.client)
        # And goes back to dashboard tab.
        self.select_navbar_element("dashboard")
        # Back to the clients
        # Disables client 1 and goes back to the dashboard.
        self.select_navbar_element("client")
        self.wait_and_click(By.LINK_TEXT, self.client)
        self.select_navbar_element("client")

        if self.client_status(self.client) == "Enabled":
            # Disables client
            self.wait_and_click(
                By.XPATH,
                '//tr[contains(td[1], "%s")]/td[5]/a[@title="Disable"]' % self.client,
            )
            if self.profile == "readonly":
                self.wait_and_click(By.LINK_TEXT, "Back")
            else:
                # Switches to dashboard, if prevented by open modal: close modal
                self.select_navbar_element(
                    "dashboard",
                    [(By.CSS_SELECTOR, "div.modal-footer > button.btn.btn-default")],
                )

        self.select_navbar_element("client")

        if self.client_status(self.client) == "Disabled":
            # Enables client
            self.wait_and_click(
                By.XPATH,
                '//tr[contains(td[1], "%s")]/td[5]/a[@title="Enable"]' % self.client,
            )
            if self.profile == "readonly":
                self.wait_and_click(By.LINK_TEXT, "Back")
            else:
                # Switches to dashboard, if prevented by open modal: close modal
                self.select_navbar_element(
                    "dashboard",
                    [(By.CSS_SELECTOR, "div.modal-footer > button.btn.btn-default")],
                )

        self.logout()

    def disabled_test_job_canceling(self):
        self.login()
        job_id = self.job_start_configured()
        self.job_cancel(job_id)
        if self.profile == "readonly":
            self.wait_and_click(By.LINK_TEXT, "Back")
        self.logout()

    def disabled_test_languages(self):
        # Goes to login page, matches found languages against predefined list, throws exception if no match
        driver = self.driver
        driver.get(self.base_url + "/auth/login")
        self.wait_and_click(By.XPATH, '//button[@data-id="locale"]')
        expected_elements = [
            "Chinese",
            "Czech",
            "Dutch/Belgium",
            "English",
            "French",
            "German",
            "Italian",
            "Russian",
            "Slovak",
            "Spanish",
            "Turkish",
        ]
        found_elements = []
        for element in self.driver.find_elements_by_xpath(
            '//ul[@aria-expanded="true"]/li[@data-original-index>"0"]/a/span[@class="text"]'
        ):
            found_elements.append(element.text)
        # Compare the counted languages against the counted directories
        for element in expected_elements:
            if element not in found_elements:
                self.logger.info("The webui misses %s" % element)

    def disabled_test_menue(self):
        self.login()
        self.select_navbar_element("director")
        self.select_navbar_element("schedule")
        self.wait_and_click(By.XPATH, '//a[contains(@href, "/schedule/status/")]')
        self.select_navbar_element("storage")
        self.select_navbar_element("client")
        self.select_navbar_element("restore")
        self.select_navbar_element("dashboard")
        self.close_alert_and_get_its_text()
        self.logout()

    def test_rerun_job(self):
        self.login()
        self.select_navbar_element("client")
        self.wait_and_click(By.LINK_TEXT, self.client)
        # Select first backup in list
        self.wait_and_click(By.XPATH, '//tr[@data-index="0"]/td[1]/a')
        # Press on rerun button
        self.wait_and_click(By.CSS_SELECTOR, "span.glyphicon.glyphicon-repeat")
        # Accept confirmation dialog
        self.driver.switch_to.alert.accept()
        if self.profile == "readonly":
            self.wait_and_click(By.LINK_TEXT, "Back")
        else:
            self.select_navbar_element(
                "dashboard",
                [(By.XPATH, "//div[@id='modal-002']/div/div/div[3]/button")],
            )
        self.logout()

    def test_restore(self):
        # Login
        self.login()
        self.select_navbar_element("restore")
        # Click on client dropdown menue and close the possible modal
        self.wait_and_click(
            By.XPATH,
            '(//button[@data-id="client"])',
            [(By.XPATH, '//div[@id="modal-001"]//button[.="Close"]')],
        )
        # Select correct client
        self.wait_and_click(By.LINK_TEXT, self.client)
        # Clicks on file and navigates through the tree
        # by using the arrow-keys.
        pathlist = self.restorefile.split("/")
        for i in pathlist[:-1]:
            self.wait_for_element(
                By.XPATH, '//a[contains(text(),"%s/")]' % i
            ).send_keys(Keys.ARROW_RIGHT)
        self.wait_for_element(
            By.XPATH, '//a[contains(text(),"%s")]' % pathlist[-1]
        ).click()
        # Submit restore
        self.wait_and_click(By.XPATH, '//button[@id="btn-form-submit"]')
        # Confirm modals
        self.wait_and_click(By.XPATH, '//div[@id="modal-003"]//button[.="OK"]')
        if self.profile == "readonly":
            self.wait_and_click(By.LINK_TEXT, "Back")
        else:
            self.wait_and_click(By.XPATH, '//div[@id="modal-002"]//button[.="Close"]')
        # Logout
        self.logout()

    def test_run_configured_job(self):
        self.login()
        self.job_start_configured()
        if self.profile == "readonly":
            self.wait_and_click(By.LINK_TEXT, "Back")
        self.logout()

    def test_run_default_job(self):
        self.login()
        self.select_navbar_element("job")
        self.wait_and_click(By.LINK_TEXT, "Run")
        # Open the job list
        self.wait_and_click(By.XPATH, '(//button[@data-id="job"])')
        # Select the first job
        self.wait_and_click(By.XPATH, '(//li[@data-original-index="1"])')
        # Start it
        self.wait_and_click(By.ID, "submit")
        if self.profile == "readonly":
            self.wait_and_click(By.LINK_TEXT, "Back")
        else:
            self.select_navbar_element("dashboard")
        self.logout()

    #
    # Methods used for testing
    #

    def client_status(self, client):
        # Wait until site and status element are loaded, check client, if not found raise exception
        self.wait.until(
            EC.presence_of_element_located(
                (By.XPATH, '//tr[contains(td[1], "%s")]/td[4]/span' % client)
            )
        )
        try:
            status = self.driver.find_element(
                By.XPATH, '//tr[contains(td[1], "%s")]/td[4]/span' % client
            ).text
        except NoSuchElementException:
            raise ClientNotFoundException(client)
        return status

    def job_cancel(self, id):
        # Wait and click cancel button
        self.wait_for_element(By.ID, "//a[@id='btn-1'][@title='Cancel']")
        self.wait_and_click(By.ID, "//a[@id='btn-1'][@title='Cancel']")

    def job_start_configured(self):
        driver = self.driver
        self.select_navbar_element("job")
        self.wait_and_click(By.LINK_TEXT, "Run")
        Select(driver.find_element(By.ID, "job")).select_by_visible_text(
            "backup-bareos-fd"
        )
        Select(driver.find_element(By.ID, "client")).select_by_visible_text(self.client)
        Select(driver.find_element(By.ID, "level")).select_by_visible_text(
            "Incremental"
        )
        # Clears the priority field and enters 5.
        self.enter_input("priority", "5")
        # Open the calendar
        self.wait_and_click(By.CSS_SELECTOR, "span.glyphicon.glyphicon-calendar")
        # Click the icon to delay jobstart by 1h six times
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        self.wait_and_click(By.XPATH, '//a[@title="Increment Hour"]')
        # Close the calendar
        self.wait_and_click(By.CSS_SELECTOR, "span.input-group-addon")
        # Submit the job
        self.wait_and_click(By.ID, "submit")

    def login(self):
        driver = self.driver
        driver.get(self.base_url + "/auth/login")
        # Currently not required in the test environment because it is preselected
        # Select(driver.find_element_by_name("director")).select_by_visible_text(
        #    "localhost-dir"
        # )
        self.enter_input("consolename", self.username)
        self.enter_input("password", self.password)

        # we want to wait until the dropdown is visible before trying
        # to select an option
        dropdown_element = WebDriverWait(driver, self.maxwait).until(
            EC.presence_of_element_located((By.ID, "locale"))
        )
        dropdown = Select(dropdown_element)
        dropdown.select_by_visible_text("English")

        self.wait_and_click(By.ID, "submit")
        try:
            # if the login is wrong, then we will get an alert div
            driver.find_element(By.XPATH, '//div[@role="alert"]')
        except:
            # if no alert was found, then we just wait for the spinner ...
            self.wait_for_spinner_absence()
        else:
            # ... otherwise (i.e. if an alert was found) the login is wronng
            raise WrongCredentialsException(self.username, self.password)

    def logout(self):
        self.wait_and_click(
            By.CSS_SELECTOR,
            "span.glyphicon.glyphicon-user",
            [(By.CSS_SELECTOR, "div.navbar-header > button")],
        )
        self.wait_and_click(By.LINK_TEXT, "Logout")
        sleep(self.sleeptime)

    def select_navbar_element(self, tab, additional_modals=None):
        tabid = "menu-topnavbar-{}".format(tab)
        # (By.CLASS_NAME, 'navbar-toggle')
        # is used, when top navbar is hidden,
        # because of low windows resolution.
        modals = []
        if additional_modals:
            modals = additional_modals
        modals += [(By.CLASS_NAME, "navbar-toggle")]
        self.wait_and_click(By.ID, tabid, modals)

    def enter_input(self, inputname, inputvalue):
        """
        Enter inputvalue into an input-element with the tag inputname.
        """
        logger = logging.getLogger()
        logger.info("Entering %r into the input-field %r.", inputvalue, inputname)
        elem = self.driver.find_element(By.NAME, inputname)
        elem.clear()
        elem.send_keys(inputvalue)

    #
    # Methods used for waiting and clicking
    #

    def getChromedriverpath(self):
        if SeleniumTest.chromedriverpath is None:
            for chromedriverpath in [
                "/usr/bin/chromedriver",
                "/usr/sbin/chromedriver",
                "/usr/local/bin/chromedriver",
                "/usr/local/sbin/chromedriver",
            ]:
                if os.path.isfile(chromedriverpath):
                    return chromedriverpath
        else:
            return SeleniumTest.chromedriverpath
        raise IOError("Chrome Driver file not found.")

    def get_duration(self, start):
        return (datetime.now() - start).total_seconds()

    def close_modals(self, modal=None):
        """
        Try to close modals, if they exist.
        If not, nothing will be done.

        @param modal: A list of elements that may exist
                      and if they exist,
                      they must be clicked.
        @type modal: List of tuples. Tuples consist out of By and by selector value.

        @return: remaining modals (modals not found and clicked)
        @rtype: List of tuples. Tuples consist out of By and by selector value.
        """
        logger = logging.getLogger()
        self.wait_for_spinner_absence()
        done = True
        if modal:
            logger.info("checking for modals %s", str(modal))
            done = False
        while not done:
            done = True
            modal_todo = modal
            modal = []
            for modal_by, modal_value in modal_todo:
                try:
                    # self.driver.switchTo().activeElement(); # required???
                    self.driver.find_element(modal_by, modal_value).click()
                except:
                    logger.info("skipped modal: %s %s not found", modal_by, modal_value)
                    modal += [(modal_by, modal_value)]
                else:
                    logger.info("closing modal %s %s", modal_by, modal_value)
                    # One modal is closed, retry the remaining modals.
                    done = False
            sleep(self.waittime)
            self.wait_for_spinner_absence()
        return modal

    def wait_and_click(self, by, value, modal=None):
        """
        @param by: Element selection type.
        @type by: By
        @param value: Element selection value.
        @type value: C{string}
        @param modal: A list of elements that may exist
                      and if they exist,
                      they must be clicked,
                      before our target element can be clicked.
                      This
        @type modal: List of tuples. Tuples consist out of by selector and value.

        @return: Selected element
        @rtype: WebElement

        @raises: FailedClickException: if element could not be found.
        """
        logger = logging.getLogger()
        logger.info("element=%s (modals=%s)", str((by, value)), str(modal))
        element = None
        starttime = datetime.now()
        seconds = 0.0
        retry = 1
        maxretry = 5
        while retry <= maxretry:
            modal = self.close_modals(modal)
            logger.info(
                "waiting for ({}, {}) (try {}/{})".format(by, value, retry, maxretry)
            )
            try:
                element = self.wait_for_element(by, value)
            except (
                ElementTimeoutException,
                ElementNotFoundException,
                ElementCoveredException,
            ) as exp:
                pass
            else:
                try:
                    element.click()
                except WebDriverException as e:
                    logger.info("WebDriverException: %s", e)
                    sleep(self.waittime)
                else:
                    logger.info(
                        "clicked %s %s (after %ss)",
                        by,
                        value,
                        self.get_duration(starttime),
                    )
                    return element
            retry += 1
        logger.error("failed to click %s %s", by, value)
        raise FailedClickException(value)

    def wait_for_element(self, by, value):
        logger = logging.getLogger()
        element = None
        logger.info("waiting for %s %s", by, value)
        try:
            element = self.wait.until(EC.element_to_be_clickable((by, value)))
        except TimeoutException:
            try:
                self.driver.find_element(by, value)
            except NoSuchElementException:
                self.driver.save_screenshot("screenshot.png")
                raise ElementNotFoundException(value)
            else:
                self.driver.save_screenshot("screenshot.png")
                raise ElementCoveredException(value)
        return element

    def wait_for_spinner_absence(self):
        logger = logging.getLogger()
        starttime = datetime.now()
        element = None
        try:
            element = WebDriverWait(self.driver, self.maxwait).until(
                EC.invisibility_of_element_located((By.ID, "spinner"))
            )
        except TimeoutException:
            raise ElementTimeoutException("spinner")
        logger.info("waited %ss", (self.get_duration(starttime)))
        return element

    def close_alert_and_get_its_text(self, accept=True):
        logger = logging.getLogger()
        try:
            alert = self.driver.switch_to_alert()
            alert_text = alert.text
        except NoAlertPresentException:
            return None
        if accept:
            alert.accept()
        else:
            alert.dismiss()
        logger.debug("alert message: %s" % (alert_text))
        return alert_text

    def tearDown(self):
        logger = logging.getLogger()
        try:
            self.driver.quit()
        except WebDriverException as e:
            logger.warn("{}: ignored".format(str(e)))

        self.assertEqual([], self.verificationErrors)

    def __get_name_of_test(self):
        return self.id().split(".", 1)[1]


def get_env():
    """
    Get attributes as environment variables,
    if not available or set use defaults.
    """

    chromedriverpath = os.environ.get("BAREOS_WEBUI_CHROMEDRIVER_PATH")
    if chromedriverpath:
        SeleniumTest.chromedriverpath = chromedriverpath

    SeleniumTest.chromeheadless = True
    chromeheadless = os.environ.get("BAREOS_WEBUI_CHROME_HEADLESS")
    if chromeheadless is not None and chromeheadless.lower() in [
        "false",
        "0",
        "n",
        "no",
        "off",
    ]:
        SeleniumTest.chromeheadless = False

    browser = os.environ.get("BAREOS_WEBUI_BROWSER")
    if browser:
        SeleniumTest.browser = browser

    resolution = os.environ.get("BAREOS_WEBUI_BROWSER_RESOLUTION")
    if resolution:
        res = [int(i.strip()) for i in resolution.split("x", 1)]
        if len(res) != 2:
            print(
                'The variable BAREOS_WEBUI_BROWSER_RESOLUTION must be set like "1200 x 800"'
            )
            sys.exit(1)
        SeleniumTest.resolution_x = res[0]
        SeleniumTest.resolution_y = res[1]

    base_url = os.environ.get("BAREOS_WEBUI_BASE_URL")
    if base_url:
        SeleniumTest.base_url = base_url.rstrip("/")

    username = os.environ.get("BAREOS_WEBUI_USERNAME")
    if username:
        SeleniumTest.username = username

    password = os.environ.get("BAREOS_WEBUI_PASSWORD")
    if password:
        SeleniumTest.password = password

    profile = os.environ.get("BAREOS_WEBUI_PROFILE")
    if profile:
        SeleniumTest.profile = profile
        print("using profile:" + profile)

    testname = os.environ.get("BAREOS_WEBUI_TESTNAME")
    if testname:
        SeleniumTest.testname = testname

    client = os.environ.get("BAREOS_WEBUI_CLIENT_NAME")
    if client:
        SeleniumTest.client = client

    restorefile = os.environ.get("BAREOS_WEBUI_RESTOREFILE")
    if restorefile:
        SeleniumTest.restorefile = restorefile

    logpath = os.environ.get("BAREOS_WEBUI_LOG_PATH")
    if logpath:
        SeleniumTest.logpath = logpath

    sleeptime = os.environ.get("BAREOS_WEBUI_DELAY")
    if sleeptime:
        SeleniumTest.sleeptime = float(sleeptime)


if __name__ == "__main__":
    get_env()
    unittest.main()
